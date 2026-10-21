// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace http {

// Fixed-size worker pool for offloading long or CPU-bound work off the
// reactor threads, so a slow request never blocks I/O for other
// connections. Handlers submit work here and hop back onto their
// connection's IoLoop (via IoLoop::Post) to write the response.
class ThreadPool
{
public:
    explicit ThreadPool(size_t numThreads, size_t queueCapacity = 128);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void enqueue(std::function<void()> task);
    // Queue slots are preallocated; a task's std::function target may allocate.
    // Rejection does not execute the task. shutdown drains and joins; call it
    // from the owning control thread, never a worker of this pool.
    bool tryEnqueue(std::function<void()> task);
    void shutdown();
    size_t failedTasks() const { return failedTasks_.load(); }
    // Tasks refused because the queue was full or the pool stopped.
    size_t rejectedTasks() const { return rejectedTasks_.load(); }
    size_t pendingTasks() const;

private:
    void workerLoop();

    std::vector<std::thread> workers_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::function<void()>> tasks_;
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t count_ = 0;
    std::atomic<size_t> failedTasks_{0};
    std::atomic<size_t> rejectedTasks_{0};
    bool stop_ = false;
};

}  // namespace http
