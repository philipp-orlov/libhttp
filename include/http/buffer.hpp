// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "http/memory_pool.hpp"

namespace http {

// Move-only byte buffer backed by its originating BufferPool (it keeps the
// pool's state alive, so it may outlive the BufferPool object). Used for
// socket read/write staging, HTTP header/body assembly, and WebSocket
// frames. Growing re-acquires a larger pooled block and copies; buffers
// are expected to be reset/reused per-connection rather than grown
// repeatedly, so this stays cheap in practice.
class Buffer
{
public:
    Buffer() = default;

    explicit Buffer(size_t initialCapacity)
    {
        if (initialCapacity > 0)
            reserve(initialCapacity);
    }

    explicit Buffer(BufferPool& pool, size_t initialCapacity = 0) : pool_(pool)
    {
        if (initialCapacity > 0)
            reserve(initialCapacity);
    }

    ~Buffer() { release(); }

    Buffer(Buffer&& other) noexcept { moveFrom(other); }

    Buffer& operator=(Buffer&& other) noexcept
    {
        if (this != &other) {
            release();
            moveFrom(other);
        }
        return *this;
    }

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    unsigned char* data() { return data_; }
    const unsigned char* data() const { return data_; }
    size_t size() const { return size_; }
    size_t capacity() const { return capacity_; }
    bool empty() const { return size_ == 0; }

    std::string_view view() const
    {
        return data_ ? std::string_view(reinterpret_cast<const char*>(data_), size_)
                     : std::string_view{};
    }

    void clear() { size_ = 0; }

    void reserve(size_t minCapacity)
    {
        if (capacity_ >= minCapacity)
            return;

        if (!pool_)
            pool_ = BufferPool::Lease(BufferPool::shared());

        size_t newCapacity = 0;
        unsigned char* newData =
            static_cast<unsigned char*>(pool_.allocate(minCapacity, newCapacity));
        if (size_ > 0)
            std::memcpy(newData, data_, size_);

        if (data_)
            pool_.deallocate(data_, capacity_);

        // size_ is deliberately left untouched -- it's the logical length
        // of the data just copied into newData, which release() (used by
        // the destructor and move-assignment, where wiping it is correct)
        // must not clobber here.
        data_ = newData;
        capacity_ = newCapacity;
    }

    void append(const void* p, size_t n)
    {
        if (n == 0)
            return;

        if (n > std::numeric_limits<size_t>::max() - size_)
            throw std::length_error("buffer size overflow");

        reserve(size_ + n);
        std::memcpy(data_ + size_, p, n);
        size_ += n;
    }

    void append(std::string_view s) { append(s.data(), s.size()); }

    // Drops the first n bytes, shifting remaining bytes to the front.
    // Used after consuming a parsed HTTP request from a keep-alive
    // connection's read buffer.
    void consumeFront(size_t n)
    {
        if (n >= size_) {
            size_ = 0;
            return;
        }
        std::memmove(data_, data_ + n, size_ - n);
        size_ -= n;
    }

    void setSize(size_t n)
    {
        if (n > capacity_)
            throw std::length_error("buffer capacity exceeded");

        size_ = n;
    }

private:
    void release()
    {
        if (data_)
            pool_.deallocate(data_, capacity_);

        data_ = nullptr;
        capacity_ = 0;
        size_ = 0;
    }

    void moveFrom(Buffer& other)
    {
        pool_ = other.pool_;
        data_ = other.data_;
        size_ = other.size_;
        capacity_ = other.capacity_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }

    BufferPool::Lease pool_;
    unsigned char* data_ = nullptr;
    size_t size_ = 0;
    size_t capacity_ = 0;
};

}  // namespace http
