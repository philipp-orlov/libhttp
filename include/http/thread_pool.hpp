// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>

namespace sched {
class ThreadPool;
}

namespace http {

// Fixed-size worker pool for offloading long or CPU-bound work off the
// reactor threads, so a slow request never blocks I/O for other
// connections. Handlers submit work here and hop back onto their
// connection's IoLoop (via IoLoop::Post) to write the response.
// A facade over develop-core's sched::ThreadPool (bounded lock-free MPMC ring, futex parking): a std::function
// task fits the ring's inline slot, so enqueueing allocates nothing beyond what the std::function itself holds.
// The queue capacity is rounded up to a power of two.
class ThreadPool
{
public:
    // `spin`: a worker that has just finished a task polls for the next one this long before it sleeps. Waking a
    // sleeping worker costs a futex round trip (tens of microseconds on a busy VM); a worker that is still polling
    // takes the next task at once and the producer's notify costs no system call. 0 (default) sleeps immediately.
    explicit ThreadPool(size_t numThreads, size_t queueCapacity = 128,
                        std::chrono::microseconds spin = std::chrono::microseconds(0));
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void enqueue(std::function<void()> task);
    // Rejection does not execute the task. shutdown drains and joins; call it
    // from the owning control thread, never a worker of this pool.
    bool tryEnqueue(std::function<void()> task);
    void shutdown();
    size_t failedTasks() const;
    // Tasks refused because the queue was full or the pool stopped.
    size_t rejectedTasks() const;
    size_t pendingTasks() const;

private:
    std::unique_ptr<sched::ThreadPool> pool_;
};

}  // namespace http
