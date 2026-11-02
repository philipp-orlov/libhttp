// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/thread_pool.hpp"

#include <stdexcept>
#include <utility>

#include "sched/thread_pool.hpp"

namespace http {

ThreadPool::ThreadPool(size_t numThreads, size_t queueCapacity, std::chrono::microseconds spin)
{
    if (!numThreads || !queueCapacity)
        throw std::invalid_argument("worker count and queue capacity must be positive");
    sched::ThreadPoolOptions options(static_cast<unsigned>(numThreads), queueCapacity);
    options.spin = spin;
    options.namePrefix = "http-worker";
    pool_ = std::make_unique<sched::ThreadPool>(std::move(options));
}

ThreadPool::~ThreadPool()
{
    shutdown();
}

void ThreadPool::shutdown()
{
    pool_->shutdown();
}

void ThreadPool::enqueue(std::function<void()> task)
{
    if (!tryEnqueue(std::move(task)))
        throw std::runtime_error("worker queue full or stopped");
}

bool ThreadPool::tryEnqueue(std::function<void()> task)
{
    if (!task)
        return false;
    // The ring moves the wrapper only on success, so a rejected task is left untouched (and not run).
    struct Run
    {
        std::function<void()> fn;
        void operator()() { fn(); }
    };
    if constexpr (sizeof(Run) <= 48)   // libstdc++ / libc++: 32 / 48 bytes, fits the inline slot
        return pool_->tryEnqueue(Run{std::move(task)});
    else                               // MSVC's std::function is larger: one boxed allocation per task
        return pool_->tryEnqueueBoxed(std::move(task));
}

size_t ThreadPool::failedTasks() const
{
    return static_cast<size_t>(pool_->failedTasks());
}

size_t ThreadPool::rejectedTasks() const
{
    return static_cast<size_t>(pool_->rejectedTasks());
}

size_t ThreadPool::pendingTasks() const
{
    return pool_->pendingTasks();
}

}  // namespace http
