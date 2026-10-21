// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/thread_pool.hpp"

namespace http {

ThreadPool::ThreadPool(size_t numThreads, size_t queueCapacity) : tasks_(queueCapacity)
{
    if (!numThreads || !queueCapacity)
        throw std::invalid_argument("worker count and queue capacity must be positive");

    workers_.reserve(numThreads);
    try {
        for (size_t index = 0; index < numThreads; ++index)
            workers_.emplace_back(&ThreadPool::workerLoop, this);
    } catch (...) {
        shutdown();
        throw;
    }
}

ThreadPool::~ThreadPool()
{
    shutdown();
}

void ThreadPool::shutdown()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) {
        if (t.joinable())
            t.join();
    }
}

void ThreadPool::enqueue(std::function<void()> task)
{
    if (!tryEnqueue(std::move(task)))
        throw std::runtime_error("worker queue full or stopped");
}

size_t ThreadPool::pendingTasks() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

bool ThreadPool::tryEnqueue(std::function<void()> task)
{
    if (!task)
        return false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_ || count_ == tasks_.size()) {
            ++rejectedTasks_;
            return false;
        }
        tasks_[tail_] = std::move(task);
        tail_ = (tail_ + 1) % tasks_.size();
        ++count_;
    }
    cv_.notify_one();
    return true;
}

void ThreadPool::workerLoop()
{
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || count_ != 0; });
            if (stop_ && count_ == 0)
                return;

            task = std::move(tasks_[head_]);
            tasks_[head_] = nullptr;
            head_ = (head_ + 1) % tasks_.size();
            --count_;
        }
        try {
            task();
        } catch (...) {
            ++failedTasks_;
        }
    }
}

}  // namespace http
