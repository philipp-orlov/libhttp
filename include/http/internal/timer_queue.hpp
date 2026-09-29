// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace http {
namespace internal {

class TimerQueue
{
public:
    using Clock = std::chrono::steady_clock;
    using Callback = std::function<void()>;

    static Clock::time_point after(std::chrono::milliseconds delay) noexcept
    {
        const auto now = Clock::now();
        if (delay.count() <= 0)
            return now;

        const auto maximum =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now);
        return delay >= maximum ? Clock::time_point::max() : now + delay;
    }

    explicit TimerQueue(size_t capacity)
    {
        if (!capacity || capacity > std::numeric_limits<uint32_t>::max())
            throw std::invalid_argument("invalid timer capacity");

        slots_.resize(capacity);
        heap_.reserve(capacity);
        free_.reserve(capacity);
        for (size_t remaining = capacity; remaining; --remaining)
            free_.push_back(remaining - 1);
    }

    uint64_t tryAdd(Clock::time_point when, Callback callback)
    {
        if (!callback)
            return 0;

        std::lock_guard<std::mutex> lock(mutex_);
        if (free_.empty())
            return 0;

        const size_t index = free_.back();
        free_.pop_back();
        auto& slot = slots_[index];
        if (++slot.generation == 0)
            ++slot.generation;

        slot.active = true;
        slot.when = when;
        slot.callback = std::move(callback);
        slot.position = heap_.size();
        heap_.push_back(index);
        armed_.store(heap_.size());
        siftUp(slot.position);
        return (static_cast<uint64_t>(slot.generation) << 32) | (index + 1);
    }

    bool refresh(uint64_t id, Clock::time_point when)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t index = find(id);
        if (index == slots_.size())
            return false;

        auto& slot = slots_[index];
        const auto previous = slot.when;
        slot.when = when;
        if (when < previous)
            siftUp(slot.position);
        else
            siftDown(slot.position);

        return true;
    }

    void cancel(uint64_t id)
    {
        Callback retired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const size_t index = find(id);
            if (index == slots_.size())
                return;

            retired = remove(index);
        }
    }

    bool popDue(Clock::time_point now, Callback& callback)
    {
        Callback retired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (heap_.empty() || slots_[heap_[0]].when > now)
                return false;

            retired = remove(heap_[0]);
        }
        callback.swap(retired);
        return true;
    }

    int waitMilliseconds(Clock::time_point now) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (heap_.empty())
            return 1000;

        const auto when = slots_[heap_[0]].when;
        if (when <= now)
            return 0;

        return static_cast<int>(std::min<int64_t>(
            1000, std::chrono::duration_cast<std::chrono::milliseconds>(when - now).count()));
    }

    size_t capacity() const noexcept { return slots_.size(); }

    // Lock-free: lets a loop skip the clock read and the lock when nothing
    // is armed, which on a busy loop is most rounds.
    bool empty() const noexcept { return armed_.load() == 0; }

private:
    struct Slot
    {
        Clock::time_point when;
        Callback callback;
        size_t position = 0;
        uint32_t generation = 0;
        bool active = false;
    };

    size_t find(uint64_t id) const noexcept
    {
        const size_t index = static_cast<uint32_t>(id) - size_t{1};
        if (index >= slots_.size() || !slots_[index].active ||
            slots_[index].generation != (id >> 32))
            return slots_.size();

        return index;
    }

    bool before(size_t first, size_t second) const noexcept
    {
        return slots_[heap_[first]].when < slots_[heap_[second]].when;
    }

    void swapPositions(size_t first, size_t second) noexcept
    {
        std::swap(heap_[first], heap_[second]);
        slots_[heap_[first]].position = first;
        slots_[heap_[second]].position = second;
    }

    void siftUp(size_t position) noexcept
    {
        while (position) {
            const size_t parent = (position - 1) / 2;
            if (!before(position, parent))
                break;

            swapPositions(position, parent);
            position = parent;
        }
    }

    void siftDown(size_t position) noexcept
    {
        while (position < heap_.size() / 2) {
            size_t child = position * 2 + 1;
            if (child + 1 < heap_.size() && before(child + 1, child))
                ++child;

            if (!before(child, position))
                break;

            swapPositions(position, child);
            position = child;
        }
    }

    Callback remove(size_t index)
    {
        auto& slot = slots_[index];
        const size_t position = slot.position;
        swapPositions(position, heap_.size() - 1);
        heap_.pop_back();
        armed_.store(heap_.size());
        if (position < heap_.size()) {
            if (position && before(position, (position - 1) / 2))
                siftUp(position);
            else
                siftDown(position);
        }
        slot.active = false;
        free_.push_back(index);
        return std::move(slot.callback);
    }

    mutable std::mutex mutex_;
    std::atomic<size_t> armed_{0};
    std::vector<Slot> slots_;
    std::vector<size_t> heap_;
    std::vector<size_t> free_;
};

}  // namespace internal
}  // namespace http