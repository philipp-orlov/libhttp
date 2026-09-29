// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "http/platform.hpp"

// Proactor-style async I/O interface. On Linux this is implemented on top
// of epoll (edge-triggered, registered once per connection; see
// io_loop_epoll.cpp for the tradeoff notes), on Windows on top of IOCP --
// callers never see the difference: AsyncRead / AsyncWrite complete on the
// owning IoLoop's thread. Linux permits bounded inline completion; callers
// must tolerate reentrancy.

namespace http {

struct IoResult
{
    size_t bytes = 0;
    bool ok = true;
    int errorCode = 0;
    // For a read: where the `bytes` landed. The caller's own buffer when it
    // gave one, otherwise a buffer the loop owns, which the caller may read
    // and modify until the callback returns and no longer.
    unsigned char* data = nullptr;
};

using IoCallback = std::function<void(IoResult)>;
using TimerCallback = std::function<void()>;
using AcceptCallback = std::function<void(socket_t)>;

class TcpConnection;
class TcpListener;

// One reactor loop, meant to be run on a single dedicated thread via
// run(). A server creates one IoLoop per worker thread.
class IoLoop
{
public:
    explicit IoLoop(size_t taskCapacity = 1024, size_t timerCapacity = 4096);
    ~IoLoop();
    IoLoop(const IoLoop&) = delete;
    IoLoop& operator=(const IoLoop&) = delete;

    // Blocks, processing I/O events until stop() is called from any thread.
    void run();
    void stop();

    // Number of completed dispatch rounds (one per wait for events). Only
    // meaningful on the loop's own thread. Lets an owner tell whether an
    // object it recycles was released during the round still in progress --
    // when frames further up the stack may still refer to it -- or earlier.
    uint64_t iteration() const;

    // The steady clock as read at the start of the current dispatch round:
    // a coarse "now" for the many callers per round that only need to know
    // roughly when they run (idle timers, say), without a clock read each.
    std::chrono::steady_clock::time_point now() const;

    // Thread-safe, bounded scheduling on this loop. post throws on full/stopped
    // queues; tryPost returns false. Callable targets may allocate independently.
    void post(std::function<void()> callback);
    bool tryPost(std::function<void()> callback);
    size_t failedTasks() const;
    // Tasks refused because the queue was full, and tasks waiting now.
    size_t rejectedTasks() const;
    size_t pendingTasks() const;

    // Thread-safe: fires callback once after `delay` from this loop's thread.
    // Returned id can be passed to cancelTimer (best-effort; a timer that
    // already fired or is mid-fire is simply ignored).
    // Timer bookkeeping is preallocated; exhaustion throws. IDs include a
    // generation so cancellation cannot affect a reused timer slot.
    uint64_t addTimer(std::chrono::milliseconds delay, TimerCallback callback);
    void cancelTimer(uint64_t id);
    bool refreshTimer(uint64_t id, std::chrono::milliseconds delay);

    struct Impl;
    Impl* impl() { return impl_.get(); }

private:
    friend class Server;
    void postControl(std::function<void()> callback);
    std::unique_ptr<Impl> impl_;
};

// A connected, non-blocking TCP socket.
class TcpConnection : public std::enable_shared_from_this<TcpConnection>
{
public:
    TcpConnection(IoLoop& loop, socket_t fd);
    ~TcpConnection();

    // Reads at most bufferCapacity bytes into buffer. callback fires
    // exactly once with bytes read (0 => peer closed) or ok=false on
    // error. Only one asyncRead may be outstanding at a time.
    //
    // With no buffer (nullptr, 0) the bytes land in a buffer the loop owns
    // and IoResult::data says where; they are the caller's to read and
    // modify until the callback returns. This is the mode for input that is
    // consumed on the spot -- it costs the connection no buffer of its own,
    // and it is the mode a backend that hands out its own receive buffers
    // (io_uring) serves natively.
    void asyncRead(unsigned char* buffer, size_t bufferCapacity, IoCallback callback);

    // Writes exactly `length` bytes from `data`; `data` must stay valid
    // until callback fires. Only one asyncWrite may be outstanding at a time.
    void asyncWrite(const unsigned char* data, size_t length, IoCallback callback);

    // Sends as much of `head` followed by `body` as the socket takes right
    // now, in one call, without queuing anything or keeping either pointer:
    // returns the number of bytes taken (possibly 0), and the caller owns the
    // rest -- typically to hand to asyncWrite from a buffer of its own. Lets
    // a large payload go out straight from the buffer it was received in.
    // Only while no asyncWrite is outstanding. A backend without an
    // immediate send simply returns 0.
    size_t tryWrite(const unsigned char* head, size_t headLength, const unsigned char* body,
                    size_t bodyLength);

    void close();
    bool isOpen() const;
    socket_t nativeHandle() const { return fd_; }
    IoLoop& loop() { return loop_; }

    // "host:port" of the peer, resolved on the first call and cached for
    // the life of the connection (an IPv6 host also contains colons, so
    // split on the last one).
    const std::string& peerAddress() const;

    struct Impl;
    Impl* impl() { return impl_.get(); }

private:
    IoLoop& loop_;
    socket_t fd_;
    std::shared_ptr<Impl> impl_;
    mutable std::string peerAddress_;
    mutable bool peerResolved_ = false;
};

// A listening TCP socket bound to a loop. Pass reusePort=true on Linux to
// let multiple loops (one per worker thread) each accept independently via
// SO_REUSEPORT, avoiding a single shared accept-lock bottleneck.
class TcpListener
{
public:
    TcpListener(IoLoop& loop, const std::string& host, uint16_t port, bool reusePort);
    ~TcpListener();

    // Begins accepting; onAccept fires for every new connection for the
    // lifetime of the listener (it re-arms itself automatically).
    void asyncAccept(AcceptCallback onAccept);
    void close();

    struct Impl;
    Impl* impl() { return impl_.get(); }

private:
    IoLoop& loop_;
    std::shared_ptr<Impl> impl_;
};

}  // namespace http
