// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/internal/timer_queue.hpp"

#include <atomic>
#include <array>
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
    using Queue = http::internal::TimerQueue;
    const auto now = Queue::Clock::now();
    Queue timers(3);
    int invoked = 0;
    trackHeap = true;
    const auto first = timers.tryAdd(now + std::chrono::seconds(3), [&] { invoked = 1; });
    const auto second = timers.tryAdd(now + std::chrono::seconds(2), [&] { invoked = 2; });
    const auto third = timers.tryAdd(now + std::chrono::seconds(1), [&] { invoked = 3; });
    if (!first || !second || !third || timers.tryAdd(now, [] {}))
        return 1;

    if (!timers.refresh(first, now))
        return 2;

    Queue::Callback due;
    if (!timers.popDue(now, due))
        return 3;

    due();
    if (invoked != 1 || timers.popDue(now, due))
        return 4;

    const auto reused = timers.tryAdd(now, [&] { invoked = 4; });
    if (!reused || reused == first || timers.refresh(first, now))
        return 5;

    timers.cancel(first);
    if (!timers.popDue(now, due))
        return 6;

    due();
    if (invoked != 4)
        return 7;

    if (!timers.refresh(third, now + std::chrono::seconds(4)) ||
        !timers.popDue(now + std::chrono::seconds(2), due))
        return 8;
    due();
    if (invoked != 2)
        return 9;

    timers.cancel(third);
    if (timers.popDue(now + std::chrono::seconds(10), due))
        return 10;

    for (int iteration = 0; iteration < 1000; ++iteration) {
        const auto id = timers.tryAdd(now, [] {});
        if (!id)
            return 11;

        timers.cancel(id);
    }
    trackHeap = false;
    if (allocations || frees)
        return 12;

    Queue randomized(31);
    struct Expected
    {
        uint64_t id = 0;
        Queue::Clock::time_point when;
    };
    std::array<Expected, 31> expected{};
    uint32_t random = 17;
    trackHeap = true;
    for (int iteration = 0; iteration < 10000; ++iteration) {
        random = random * 1664525u + 1013904223u;
        const size_t index = random % expected.size();
        random = random * 1664525u + 1013904223u;
        const auto when = now + std::chrono::milliseconds(random % 1000);
        auto& entry = expected[index];
        if (!entry.id) {
            entry.id =
                randomized.tryAdd(when, [index, &invoked] { invoked = static_cast<int>(index); });
            entry.when = when;
            if (!entry.id)
                return 13;
        } else if (random & 16) {
            if (!randomized.refresh(entry.id, when))
                return 14;

            entry.when = when;
        } else {
            randomized.cancel(entry.id);
            entry.id = 0;
        }
        auto earliest = Queue::Clock::time_point::max();
        for (const auto& candidate : expected)
            if (candidate.id && candidate.when < earliest)
                earliest = candidate.when;

        const auto cutoff = now + std::chrono::milliseconds(random % 1000);
        const bool popped = randomized.popDue(cutoff, due);
        if (popped != (earliest <= cutoff))
            return 15;

        if (popped) {
            due();
            auto& fired = expected[static_cast<size_t>(invoked)];
            if (!fired.id || fired.when != earliest)
                return 16;

            fired.id = 0;
        }
    }
    trackHeap = false;
    if (allocations || frees)
        return 17;

    std::puts(
        "timers: bounded slots, refresh ordering, stale IDs, reuse and zero heap activity passed");
}