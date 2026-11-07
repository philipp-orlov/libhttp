// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// WebSocket frames waiting behind a socket that has not taken them yet, as
// pooled blocks: a frame sent with WebSocketConnection::send(Buffer&&) is not
// copied into the session's output string, it waits here (header and payload)
// and goes to the socket in place. A fixed-capacity ring, allocated once at
// its first use and kept, so a connection that sends large frames does not
// churn the heap.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "http/buffer.hpp"

namespace http::internal {

struct OutFrame
{
    char header[10];
    uint8_t headerSize = 0;  // 0: `payload` already holds the whole frame (a copy queued behind others)
    Buffer payload;
    size_t sent = 0;  // bytes of header + payload the socket has taken

    size_t total() const { return headerSize + payload.size(); }
    size_t remaining() const { return total() - sent; }
};

class OutFrameQueue
{
public:
    size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }
    // Bytes not yet taken by the socket, over every queued frame.
    size_t pendingBytes() const { return pendingBytes_; }

    // Room for one more frame, growing the ring as needed up to `limit`
    // frames, the server's frame limit. False: the queue is at the limit.
    // Grown rather than sized to the limit at once, which would cost every
    // connection that ever queues a frame the whole limit's worth of slots.
    bool makeRoom(size_t limit)
    {
        if (count_ < slots_.size())
            return true;
        if (count_ >= limit)
            return false;
        std::vector<OutFrame> grown(std::min(std::max<size_t>(8, slots_.size() * 2), limit));
        for (size_t i = 0; i < count_; ++i)
            grown[i] = std::move(slots_[(head_ + i) % slots_.size()]);
        slots_.swap(grown);
        head_ = 0;
        return true;
    }
    // Gives the ring's storage back; only when empty.
    void release()
    {
        if (count_ == 0) {
            std::vector<OutFrame>().swap(slots_);
            head_ = 0;
        }
    }

    void push(OutFrame&& frame)
    {
        pendingBytes_ += frame.remaining();
        slots_[(head_ + count_) % slots_.size()] = std::move(frame);
        ++count_;
    }
    OutFrame& front() { return slots_[head_]; }
    // The socket took `bytes` of the front frame.
    void advance(size_t bytes)
    {
        front().sent += bytes;
        pendingBytes_ -= bytes;
    }
    void pop()
    {
        slots_[head_] = OutFrame();  // releases the block to its pool
        head_ = (head_ + 1) % slots_.size();
        --count_;
    }
    void clear()
    {
        while (count_)
            pop();
        head_ = 0;
        pendingBytes_ = 0;
    }

private:
    std::vector<OutFrame> slots_;
    size_t head_ = 0;
    size_t count_ = 0;
    size_t pendingBytes_ = 0;
};

}  // namespace http::internal
