// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/internal/task_queue.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>

static std::atomic<bool> trackHeap{false};
static std::atomic<size_t> allocations{0};
static std::atomic<size_t> frees{0};

void* operator new(size_t bytes)
{
    if (trackHeap)
        ++allocations;

    if (void* pointer = std::malloc(bytes ? bytes : 1))
        return pointer;

    throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept
{
    if (pointer && trackHeap)
        ++frees;

    std::free(pointer);
}
void operator delete(void* pointer, size_t) noexcept
{
    ::operator delete(pointer);
}

int main()
{
    http::internal::TaskQueue tasks(2);
    struct State
    {
        http::internal::TaskQueue& tasks;
        int invoked = 0;
        bool nestedAccepted = false;
    } state{tasks};
    trackHeap = true;
    if (!tasks.tryPush([&state] {
            ++state.invoked;
            state.nestedAccepted = state.tasks.tryPush([&state] { ++state.invoked; });
        }) ||
        !tasks.tryPush([&state] { ++state.invoked; }) || tasks.tryPush([] {}))
        return 1;
    if (!tasks.tryPush([&] { tasks.close(); }, true))
        return 2;

    tasks.drain();
    if (state.invoked != 2 || !state.nestedAccepted || !tasks.closed() || !tasks.hasPending() ||
        tasks.tryPush([] {}))
        return 3;

    tasks.drain();
    trackHeap = false;
    if (state.invoked != 3 || tasks.hasPending() || allocations || frees)
        return 4;

    http::internal::TaskQueue failures(2);
    failures.tryPush([] { throw 7; });
    failures.tryPush([&state] { ++state.invoked; });
    failures.drain();
    if (failures.failedTasks() != 1 || state.invoked != 4)
        return 5;

    std::puts(
        "reactor tasks: bounded slots, reserved controls, recursive posting, shutdown drain and "
        "exception containment passed");
}