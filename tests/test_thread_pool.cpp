// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/thread_pool.hpp"

#include <future>
#include <cstdio>

int main()
{
    http::ThreadPool pool(1, 2);
    std::promise<void> started;
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::atomic<int> completed{0};
    pool.enqueue([&] {
        started.set_value();
        gate.wait();
        ++completed;
    });
    started.get_future().wait();
    if (!pool.tryEnqueue([&] { ++completed; }))
        return 1;

    if (!pool.tryEnqueue([] { throw std::runtime_error("test"); }))
        return 2;

    if (pool.tryEnqueue([] {}))
        return 3;

    release.set_value();
    pool.shutdown();
    if (completed != 2 || pool.failedTasks() != 1 || pool.tryEnqueue([] {}))
        return 4;

    std::puts("worker pool: capacity, drain, exception containment, stopped rejection passed");
}