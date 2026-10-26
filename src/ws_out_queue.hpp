// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// WebSocket frames waiting behind a socket that has not taken them yet, as
// pooled blocks: a frame sent with WebSocketConnection::send(Buffer&&) is not
// copied into the session's output string, it waits here (header and payload)
// and goes to the socket in place. A fixed-capacity ring, allocated once at
// its first use and kept, so a connection that sends large frames does not
// churn the heap.
#pragma once

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

    // The ring is sized on first use and never resized: `capacity` is the
    // server's frame limit, so a push beyond it is refused by the caller.
    void reserve(size_t capacity)
    {
        if (slots_.empty())
            slots_.resize(capacity ? capacity : 1);
    }
    bool full() const { return count_ == slots_.size(); }

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
