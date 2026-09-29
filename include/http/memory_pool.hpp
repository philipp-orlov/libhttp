// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

// Owner-scoped bounded slabs. Cross-thread acquire/release is serialized;
// release only links a block into its size class and updates counters.

namespace http {

// Optional startup-only allocator pair. Storage must be max_align_t aligned.
using MemoryAllocateFn = std::function<void*(size_t size)>;
using MemoryDeallocateFn = std::function<void(void* ptr, size_t size)>;

// Unsynchronized primitive; BufferPool supplies synchronization and budgets.
class SlabPool
{
public:
    SlabPool(size_t blockSize, size_t blocksPerSlab, size_t maxSlabs = 64);
    ~SlabPool();

    SlabPool(const SlabPool&) = delete;
    SlabPool& operator=(const SlabPool&) = delete;

    void* acquire();
    void release(void* p) noexcept;
    void provision(size_t slabs);
    size_t slabBytes() const { return block_size_ * blocks_per_slab_; }
    bool empty() const { return free_list_ == nullptr; }

    size_t blockSize() const { return block_size_; }
    size_t slabCount() const { return slabs_.size(); }

private:
    struct FreeNode
    {
        FreeNode* next;
    };

    void addSlab();
    size_t blockIndex(const void* pointer) const noexcept;

    size_t block_size_;
    size_t blocks_per_slab_;
    size_t max_slabs_;
    FreeNode* free_list_ = nullptr;
    std::vector<void*>
        slabs_;  // raw allocations, freed via the installed allocator hook (or the heap)
    std::vector<uint64_t> in_use_;
};

// A pool's leases (Buffers) may outlive the BufferPool object: the state
// behind it is shared, and its slabs are freed when the last lease returns.
// A bookkeeping error (a block returned twice, to the wrong class, or to a
// pool that never issued it) is logged and the block left out of circulation;
// a Debug build aborts instead, to catch it where it happens.
class BufferPool
{
public:
    BufferPool();
    ~BufferPool();
    BufferPool(const BufferPool&) = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    // The process-lifetime pool a Buffer constructed without an explicit
    // owner draws from. Servers own their own pool instead.
    static BufferPool& shared();

    // Configure before starting threads; changes after first slab allocation
    // are rejected. Callback state must outlive process teardown.
    static void setAllocator(MemoryAllocateFn allocate, MemoryDeallocateFn deallocate);

    // Returns a block whose capacity is >= size (rounded up to a size
    // class), writing the actual capacity to outCapacity. When the class has
    // no free block and cannot grow (sealed, at its slab quota or over the
    // byte budget) a free block of one of the next few larger classes is lent
    // instead; it goes back to that class on release. Exhaustion throws
    // bad_alloc; there is no unpooled oversized allocation fallback.
    void* allocate(size_t size, size_t& outCapacity);
    void deallocate(void* p, size_t capacity) noexcept;
    void* tryAllocate(size_t size, size_t& capacity) noexcept;

    static constexpr size_t kSizeClasses[] = {512,      4096,     16384,     65536,    262144,
                                              1048576,  2097152,  4194304,   8388608,  16777216,
                                              33554432, 67108864, 134217728, 268435456};
    static constexpr size_t kNumClasses = sizeof(kSizeClasses) / sizeof(size_t);

    struct Config
    {
        size_t maxBytes = 512ull * 1024 * 1024;
        size_t maxSlabsPerClass = 64;
        std::array<size_t, kNumClasses> initialSlabs{};
        bool sealed = false;
    };
    // Slabs are never returned before destruction, so backingBytes is the
    // high-water mark. `borrowed` counts requests served from a larger class,
    // `refused` those that found nothing to give.
    struct Stats
    {
        size_t backingBytes;
        size_t outstandingBlocks;
        size_t allocations;
        size_t borrowed;
        size_t refused;
    };
    void configure(const Config& config);
    Stats stats() const;

    class Core;  // the pool's state, shared with its leases (memory_pool.cpp)

    // A share in a pool's state, held by a Buffer: it keeps the slabs alive
    // for as long as any block of the pool is outstanding.
    class Lease
    {
    public:
        Lease() = default;
        explicit Lease(BufferPool& pool) : core_(pool.core_) {}
        explicit operator bool() const noexcept { return core_ != nullptr; }
        void* allocate(size_t size, size_t& capacity) const;
        void deallocate(void* p, size_t capacity) const noexcept;

    private:
        std::shared_ptr<Core> core_;
    };

private:
    std::shared_ptr<Core> core_;
};

}  // namespace http
