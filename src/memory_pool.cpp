// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/memory_pool.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>

#include "http/logger.hpp"

namespace http {

namespace {
MemoryAllocateFn g_customAllocate;
MemoryDeallocateFn g_customDeallocate;
std::atomic<bool> allocatorFrozen{false};

// How many classes above its own a request may borrow from.
constexpr size_t kMaxBorrowClasses = 4;

// Fewer, larger slabs for the big size classes -- an 8MB slab of 1MB blocks
// is plenty of headroom without over-committing memory for classes that are
// used less often (large image bodies).
constexpr size_t kBlocksPerSlab[BufferPool::kNumClasses] = {256, 128, 64, 32, 32, 16, 16, 8, 8,
                                                            8,   4,   4,  2,  2,  1,  1,  1, 1};

void* allocateRaw(size_t size)
{
    allocatorFrozen = true;
    void* pointer = g_customAllocate ? g_customAllocate(size) : std::malloc(size);
    if (!pointer)
        throw std::bad_alloc();

    return pointer;
}

void deallocateRaw(void* p, size_t size)
{
    if (!p)
        return;

    if (g_customDeallocate) {
        g_customDeallocate(p, size);
        return;
    }
    std::free(p);
}

// A bookkeeping error: the pool's records disagree with what the caller did.
// Release builds log it and go on (the block stays out of circulation, which
// wastes a block but cannot hand it out twice); Debug builds abort so the
// mistake is caught where it was made.
void misuse(const char* what) noexcept
{
    HTTP_LOG_ERROR("buffer pool: %s", what);
#ifndef NDEBUG
    std::terminate();
#endif
}
}  // namespace

void BufferPool::setAllocator(MemoryAllocateFn allocate, MemoryDeallocateFn deallocate)
{
    if (allocatorFrozen)
        throw std::logic_error("buffer allocator is already in use");

    if (static_cast<bool>(allocate) != static_cast<bool>(deallocate)) {
        throw std::invalid_argument("both allocator callbacks are required");
    }
    g_customAllocate = std::move(allocate);
    g_customDeallocate = std::move(deallocate);
}

SlabPool::SlabPool(size_t blockSize, size_t blocksPerSlab, size_t maxSlabs)
    : block_size_(std::max(blockSize, sizeof(FreeNode))),
      blocks_per_slab_(blocksPerSlab),
      max_slabs_(maxSlabs)
{
    constexpr size_t alignment = alignof(std::max_align_t);
    if (block_size_ > std::numeric_limits<size_t>::max() - alignment + 1)
        throw std::bad_alloc();

    block_size_ = (block_size_ + alignment - 1) / alignment * alignment;
    if (!blocks_per_slab_ || block_size_ > std::numeric_limits<size_t>::max() / blocks_per_slab_) {
        throw std::invalid_argument("invalid slab size");
    }
    if (max_slabs_ > (std::numeric_limits<size_t>::max() - 63) / blocks_per_slab_) {
        throw std::invalid_argument("invalid slab bookkeeping size");
    }
    slabs_.reserve(max_slabs_);
    slabsByAddress_.reserve(max_slabs_);
    in_use_.resize((max_slabs_ * blocks_per_slab_ + 63) / 64);
}

SlabPool::~SlabPool()
{
    for (uint64_t occupied : in_use_) {
        if (occupied) {
            // A block is still marked in use: whoever holds it may still write
            // to it, so its slab is left allocated rather than freed under it.
            misuse("slab pool destroyed with blocks still in use; their memory is leaked");
            return;
        }
    }
    for (void* slab : slabs_)
        deallocateRaw(slab, block_size_ * blocks_per_slab_);
}

void SlabPool::addSlab()
{
    if (slabs_.size() == max_slabs_)
        throw std::bad_alloc();

    void* slab = allocateRaw(block_size_ * blocks_per_slab_);
    if (reinterpret_cast<size_t>(slab) % alignof(std::max_align_t) != 0) {
        deallocateRaw(slab, slabBytes());
        throw std::bad_alloc();
    }
    slabs_.push_back(slab);
    const std::pair<uintptr_t, size_t> entry{reinterpret_cast<uintptr_t>(slab), slabs_.size() - 1};
    slabsByAddress_.insert(std::upper_bound(slabsByAddress_.begin(), slabsByAddress_.end(), entry),
                           entry);
    unsigned char* base = static_cast<unsigned char*>(slab);
    for (size_t i = 0; i < blocks_per_slab_; ++i) {
        auto* node = new (base + i * block_size_)
            FreeNode{free_list_, (slabs_.size() - 1) * blocks_per_slab_ + i};
        free_list_ = node;
    }
}

void SlabPool::provision(size_t slabs)
{
    if (slabs > max_slabs_)
        throw std::invalid_argument("slab capacity exceeded");

    while (slabs_.size() < slabs)
        addSlab();
}

void* SlabPool::acquire()
{
    if (!free_list_)
        addSlab();

    FreeNode* node = free_list_;
    const size_t index = node->index;
    const size_t slab = index / blocks_per_slab_;
    if (slab >= slabs_.size() ||
        static_cast<unsigned char*>(slabs_[slab]) + (index % blocks_per_slab_) * block_size_ !=
            reinterpret_cast<unsigned char*>(node)) {
        misuse("free list holds a block that belongs to no slab");
        throw std::logic_error("corrupt buffer pool free list");
    }
    const uint64_t mask = uint64_t{1} << (index % 64);
    if (in_use_[index / 64] & mask) {
        misuse("free list holds a block that is marked in use");
        throw std::logic_error("corrupt buffer pool free list");
    }
    in_use_[index / 64] |= mask;
    free_list_ = node->next;
    return node;
}

void SlabPool::release(void* p) noexcept
{
    if (!p)
        return;

    const size_t index = blockIndex(p);
    if (index == std::numeric_limits<size_t>::max()) {
        misuse("block returned to a pool that did not issue it; ignored");
        return;
    }
    const uint64_t mask = uint64_t{1} << (index % 64);
    if (!(in_use_[index / 64] & mask)) {
        misuse("block returned twice; ignored");
        return;
    }
    in_use_[index / 64] &= ~mask;
    free_list_ = new (p) FreeNode{free_list_, index};
}

size_t SlabPool::blockIndex(const void* pointer) const noexcept
{
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    // The last slab starting at or below the address is the only candidate.
    auto after = std::upper_bound(slabsByAddress_.begin(), slabsByAddress_.end(), address,
                                  [](uintptr_t value, const std::pair<uintptr_t, size_t>& slab) {
                                      return value < slab.first;
                                  });
    if (after == slabsByAddress_.begin())
        return std::numeric_limits<size_t>::max();

    --after;
    const auto offset = address - after->first;
    if (offset < slabBytes() && offset % block_size_ == 0)
        return after->second * blocks_per_slab_ + offset / block_size_;

    return std::numeric_limits<size_t>::max();
}

// The state of a BufferPool, shared with the Buffers that lease from it.
class BufferPool::Core
{
public:
    Core()
    {
        for (size_t i = 0; i < kNumClasses; ++i)
            pools_[i] = std::make_unique<SlabPool>(kSizeClasses[i], kBlocksPerSlab[i]);
    }

    ~Core()
    {
        if (outstanding_)
            misuse("pool state destroyed with blocks still outstanding");
    }

    size_t outstanding() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return outstanding_;
    }

    void* tryAllocate(size_t size, size_t& capacity) noexcept
    {
        capacity = 0;
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            configured_ = true;
            size_t index = findClass(size);
            if (index == kNumClasses)
                return nullptr;

            SlabPool* pool = pools_[index].get();

            bool grow = pool->empty();
            if (grow && (config_.sealed || pool->slabCount() >= config_.maxSlabsPerClass ||
                         pool->slabBytes() > config_.maxBytes - backing_bytes_)) {
                // Cannot grow: lend a free block of a slightly larger class
                // rather than fail, so a burst in one class cannot starve the
                // others for the life of the process.
                grow = false;
                SlabPool* lender = nullptr;
                const size_t last = std::min(kNumClasses, index + 1 + kMaxBorrowClasses);
                for (size_t other = index + 1; other < last && !lender; ++other)
                    if (!pools_[other]->empty())
                        lender = pools_[other].get();

                if (!lender) {
                    ++refused_;
                    return nullptr;
                }
                pool = lender;
                ++borrowed_;
            }
            void* pointer = pool->acquire();
            if (grow)
                backing_bytes_ += pool->slabBytes();

            capacity = pool->blockSize();
            ++outstanding_;
            ++allocations_;
            return pointer;
        } catch (...) {
            return nullptr;
        }
    }

    void deallocate(void* p, size_t capacity) noexcept
    {
        if (!p)
            return;

        std::lock_guard<std::mutex> lock(mutex_);
        const size_t index = findClass(capacity);
        if (index < kNumClasses) {
            // capacity for a pooled block always equals a size class exactly,
            // so this always matches the pool it was allocated from.
            SlabPool* pool = pools_[index].get();
            if (capacity == pool->blockSize() && outstanding_) {
                pool->release(p);
                --outstanding_;
                return;
            }
        }
        misuse("block returned with a capacity that is no size class of this pool; ignored");
    }

    void configure(const Config& config)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (configured_)
            throw std::logic_error("buffer pool is already in use");

        std::array<std::unique_ptr<SlabPool>, kNumClasses> prepared;
        size_t total = 0;
        for (size_t index = 0; index < kNumClasses; ++index) {
            if (config.initialSlabs[index] > config.maxSlabsPerClass)
                throw std::invalid_argument("initial slab count exceeds limit");

            prepared[index] = std::make_unique<SlabPool>(kSizeClasses[index], kBlocksPerSlab[index],
                                                         config.maxSlabsPerClass);
            if (config.initialSlabs[index] >
                (config.maxBytes - total) / prepared[index]->slabBytes()) {
                throw std::invalid_argument("initial slabs exceed memory budget");
            }
            total += config.initialSlabs[index] * prepared[index]->slabBytes();
        }
        for (size_t index = 0; index < kNumClasses; ++index)
            prepared[index]->provision(config.initialSlabs[index]);

        pools_ = std::move(prepared);
        config_ = config;
        backing_bytes_ = total;
        configured_ = true;
    }

    Stats stats() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return {backing_bytes_, outstanding_, allocations_, borrowed_, refused_};
    }

private:
    // A binary search: the class table is sorted.
    static size_t findClass(size_t size)
    {
        return static_cast<size_t>(
            std::lower_bound(kSizeClasses, kSizeClasses + kNumClasses, size) - kSizeClasses);
    }

    std::array<std::unique_ptr<SlabPool>, kNumClasses> pools_;
    mutable std::mutex mutex_;
    Config config_;
    size_t backing_bytes_ = 0;
    size_t outstanding_ = 0;
    size_t allocations_ = 0;
    size_t borrowed_ = 0;
    size_t refused_ = 0;
    bool configured_ = false;
};

BufferPool::BufferPool() : core_(std::make_shared<Core>()) {}

BufferPool::~BufferPool()
{
    // Buffers still holding blocks keep the state (and the slabs) alive until
    // the last one is released; say so, since it usually means a leaked request.
    if (const size_t outstanding = core_->outstanding())
        HTTP_LOG_WARN("buffer pool destroyed with %zu blocks in use; they are freed when returned",
                      outstanding);
}

BufferPool& BufferPool::shared()
{
    static BufferPool instance;
    return instance;
}

void* BufferPool::allocate(size_t size, size_t& outCapacity)
{
    if (void* pointer = tryAllocate(size, outCapacity))
        return pointer;

    throw std::bad_alloc();
}

void BufferPool::deallocate(void* p, size_t capacity) noexcept
{
    core_->deallocate(p, capacity);
}

void* BufferPool::tryAllocate(size_t size, size_t& capacity) noexcept
{
    return core_->tryAllocate(size, capacity);
}

void BufferPool::configure(const Config& config)
{
    core_->configure(config);
}

BufferPool::Stats BufferPool::stats() const
{
    return core_->stats();
}

void* BufferPool::Lease::allocate(size_t size, size_t& capacity) const
{
    if (void* pointer = core_->tryAllocate(size, capacity))
        return pointer;

    throw std::bad_alloc();
}

void BufferPool::Lease::deallocate(void* p, size_t capacity) const noexcept
{
    core_->deallocate(p, capacity);
}

}  // namespace http
