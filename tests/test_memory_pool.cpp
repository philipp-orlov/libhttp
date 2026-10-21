// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/buffer.hpp"

#include <atomic>
#include <array>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>
#include <new>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

static std::atomic<bool> trackHeap{false};
static std::atomic<size_t> heapAllocations{0};
static std::atomic<size_t> heapFrees{0};

void* operator new(size_t bytes)
{
    if (trackHeap)
        ++heapAllocations;

    if (void* pointer = std::malloc(bytes ? bytes : 1))
        return pointer;

    throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept
{
    if (pointer && trackHeap)
        ++heapFrees;

    std::free(pointer);
}
void operator delete(void* pointer, size_t) noexcept
{
    ::operator delete(pointer);
}

int main()
{
    static std::atomic<size_t> backingAllocations{0};
    static std::atomic<size_t> backingFrees{0};
    http::BufferPool::setAllocator(
        [](size_t bytes) {
            ++backingAllocations;
            return std::malloc(bytes);
        },
        [](void* pointer, size_t) {
            ++backingFrees;
            std::free(pointer);
        });
    http::BufferPool::Config config;
    config.initialSlabs[3] = 1;
    config.sealed = true;
    http::BufferPool::shared().configure(config);
    for (int iteration = 0; iteration < 96; ++iteration) {
        http::Buffer body(65536);
        std::thread worker([body = std::move(body)] {});
        worker.join();
    }
    http::Buffer survivor;
    std::thread producer([&] { survivor.reserve(65536); });
    producer.join();
    survivor.append("valid", 5);
    if (survivor.view() != "valid")
        return 1;

    survivor = http::Buffer{};
    std::vector<http::Buffer> occupied;
    occupied.reserve(32);
    for (int index = 0; index < 32; ++index)
        occupied.emplace_back(65536);

    size_t capacity = 0;
    trackHeap = true;
    if (http::BufferPool::shared().tryAllocate(65536, capacity))
        return 2;

    occupied.clear();
    trackHeap = false;
    if (heapAllocations || heapFrees || backingFrees)
        return 5;

    if (backingAllocations != 1 || http::BufferPool::shared().stats().outstandingBlocks != 0)
        return 3;

    if (http::BufferPool::shared().tryAllocate(16777217, capacity))
        return 4;

    {
        http::BufferPool first;
        http::BufferPool second;
        first.configure(config);
        second.configure(config);
        http::Buffer original(first, 65536);
        original.append("owned", 5);
        http::Buffer destination(second, 65536);
        trackHeap = true;
        destination = std::move(original);
        trackHeap = false;
        if (heapAllocations || heapFrees || second.stats().outstandingBlocks ||
            first.stats().outstandingBlocks != 1 || destination.view() != "owned")
            return 6;
        std::thread worker([body = std::move(destination)] {});
        worker.join();
        if (first.stats().outstandingBlocks)
            return 7;

#ifndef _WIN32
        for (int invalidReturn = 0; invalidReturn < 3; ++invalidReturn) {
            const pid_t child = fork();
            if (child < 0)
                return 8;

            if (child == 0) {
                std::set_terminate([] { std::_Exit(97); });
                size_t firstCapacity = 0;
                void* pointer = first.allocate(65536, firstCapacity);
                size_t secondCapacity = 0;
                first.allocate(65536, secondCapacity);
                second.allocate(65536, secondCapacity);
                if (invalidReturn == 0)
                    second.deallocate(pointer, firstCapacity);

                if (invalidReturn == 1) {
                    first.deallocate(pointer, firstCapacity);
                    first.deallocate(pointer, firstCapacity);
                }
                if (invalidReturn == 2)
                    first.deallocate(static_cast<unsigned char*>(pointer) + 1, firstCapacity);

                std::_Exit(98);
            }
            int status = 0;
#ifdef NDEBUG
            // Release: the misuse is logged and ignored, the process carries on.
            const int expectedExit = 98;
#else
            // Debug: the misuse aborts where it happens.
            const int expectedExit = 97;
#endif
            if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
                WEXITSTATUS(status) != expectedExit)
                return 9;
        }
#endif
    }
    if (backingFrees != 2)
        return 10;

    {
        // A Buffer may outlive the BufferPool object it came from: the slabs
        // stay until the last block is returned, then go.
        const size_t freesBefore = backingFrees;
        http::Buffer orphan;
        {
            http::BufferPool temporary;
            temporary.configure(config);
            orphan = http::Buffer(temporary, 65536);
            orphan.append("alive", 5);
        }
        if (orphan.view() != "alive" || backingFrees != freesBefore)
            return 13;

        orphan.append(" and well", 9);
        orphan = http::Buffer{};
        if (backingFrees != freesBefore + 1)
            return 14;
    }

    {
        // A sealed pool lends a free block of a larger class instead of failing.
        http::BufferPool lending;
        http::BufferPool::Config lendingConfig;
        lendingConfig.initialSlabs[2] = 1;
        lendingConfig.sealed = true;
        lending.configure(lendingConfig);
        size_t lentCapacity = 0;
        void* lent = lending.tryAllocate(100, lentCapacity);
        if (!lent || lentCapacity != 16384 || lending.stats().borrowed != 1)
            return 11;

        lending.deallocate(lent, lentCapacity);
        size_t none = 0;
        if (lending.tryAllocate(1048576, none) || lending.stats().refused != 1 ||
            lending.stats().outstandingBlocks != 0)
            return 12;
    }

    {
        http::SlabPool slabs(512, 16, 64);
        slabs.provision(64);
        std::array<void*, 1024> blocks{};
        const auto allocationsBefore = backingAllocations.load();
        const auto freesBefore = backingFrees.load();
        heapAllocations = 0;
        heapFrees = 0;
        trackHeap = true;
        for (unsigned round = 0; round < 10000; ++round) {
            for (size_t i = 0; i < blocks.size(); ++i) {
                blocks[i] = slabs.acquire();
                std::memset(blocks[i], static_cast<int>(i % 251), slabs.blockSize());
            }
            for (size_t i = 0; i < blocks.size(); ++i) {
                const size_t at = (i * 37) % blocks.size();
                const auto* bytes = static_cast<const unsigned char*>(blocks[at]);
                for (size_t byte = 0; byte < slabs.blockSize(); ++byte)
                    if (bytes[byte] != at % 251)
                        return 15;
                slabs.release(blocks[at]);
            }
        }
        trackHeap = false;
        if (heapAllocations || heapFrees || backingAllocations != allocationsBefore ||
            backingFrees != freesBefore || slabs.slabCount() != 64)
            return 16;
    }

    {
        // The classes between 64 KiB and 1 MiB, found by size.
        static_assert(http::BufferPool::classIndex(1) == 0, "smallest class");
        static_assert(http::BufferPool::classIndex(16384) == 2, "16 KiB class");
        static_assert(http::BufferPool::classIndex(65536) == 3, "64 KiB class");
        static_assert(
            http::BufferPool::kSizeClasses[http::BufferPool::classIndex(300 * 1024)] == 393216,
            "300 KiB takes a 384 KiB block");
        static_assert(http::BufferPool::classIndex(268435457) == http::BufferPool::kNumClasses,
                      "nothing above the largest class");
        http::BufferPool sized;
        size_t blockCapacity = 0;
        void* block = sized.allocate(70 * 1000, blockCapacity);
        if (blockCapacity != 98304)
            return 20;
        sized.deallocate(block, blockCapacity);
        if (sized.stats().outstandingBlocks != 0)
            return 21;
    }

    {
        // Release finds a block's slab among many, in any order.
        http::SlabPool slabs(4096, 2, 64);
        std::vector<void*> blocks;
        for (int index = 0; index < 128; ++index)
            blocks.push_back(slabs.acquire());
        for (size_t index = 0; index < blocks.size(); ++index)
            slabs.release(blocks[(index * 53) % blocks.size()]);
        for (int index = 0; index < 128; ++index)
            blocks[static_cast<size_t>(index)] = slabs.acquire();
        if (slabs.slabCount() != 64)
            return 22;
        for (void* pointer : blocks)
            slabs.release(pointer);
    }

    {
        http::BufferPool owner;
        http::Buffer buffer(owner);
        const std::string payload(500, 'x');
        buffer.append(payload);
        buffer.append(buffer.view());
        if (buffer.view() != payload + payload)
            return 17;
        buffer.append(buffer.view().substr(3, 100));
        if (buffer.view() != payload + payload + payload.substr(3, 100))
            return 18;
        http::Buffer slice(owner);
        slice.append(payload);
        slice.append(slice.view().substr(17));
        if (slice.view() != payload + payload.substr(17))
            return 19;
    }

    std::puts(
        "memory pool: cross-thread reuse, explicit owners, thread exit, sealed exhaustion passed");
}