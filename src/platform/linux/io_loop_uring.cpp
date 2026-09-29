// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/io_loop.hpp"
#include "http/internal/task_queue.hpp"
#include "http/internal/timer_queue.hpp"

#include <liburing.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netdb.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#include "http/logger.hpp"

// Linux backend on io_uring (built instead of the epoll one with
// HTTP_WITH_IO_URING=ON; needs liburing and a 6.x kernel).
//
// Design notes and the tradeoffs behind them:
//  - Nothing is attempted inline. A read or write becomes a submission
//    queue entry and completes on a later round; all entries queued during
//    a round go to the kernel in the one io_uring_enter that also waits for
//    completions. Where the epoll backend pays a recv, a send and a share of
//    an epoll_wait per request, this one pays a share of a single syscall
//    per round for every connection that had something to do -- the whole
//    point of the backend. Latency per request is one round higher; on a
//    busy loop a round is microseconds.
//  - Reads issued without a buffer (the common, consume-on-the-spot mode of
//    IoLoop's asyncRead) select their buffer from a group the loop provides
//    to the kernel: the kernel picks one when data arrives, the completion
//    says which, the callback consumes it and the buffer goes straight back
//    to the group. The group is filled and refilled with
//    IORING_OP_PROVIDE_BUFFERS entries -- batched with everything else, so
//    no syscall of their own -- rather than a registered buffer ring, which
//    some distribution kernels refuse (Ubuntu's 6.8 answers EINVAL). Reads
//    into a caller's buffer (a request body, the rest of a WebSocket frame)
//    are plain recvs into that memory.
//  - One-shot operations, re-armed per asyncRead/asyncWrite, rather than
//    multishot recv: the callers only want data when they ask for it, and a
//    multishot recv would have to be buffered on their behalf and paused
//    for backpressure. Accept is multishot, since a listener always wants
//    the next connection.
//  - Closing a connection shuts the socket down, cancels its operations by
//    their user_data and drops its handler token, so a completion that
//    arrives later finds no one and only returns its buffer. Cancelling by
//    descriptor would be wrong: the number may already belong to a new
//    connection.
//  - Task and timer plumbing (postControl, wakeUp, the sleeping flag) is the
//    epoll backend's; the wakeup eventfd is read through the ring.
//  - Known kernel behaviour (seen on Ubuntu's 6.8, reproduced with a bare
//    liburing ring that only waits): while a thread waits in io_uring_enter,
//    another thread of the same process blocked in a socket read with a
//    receive timeout may get EINTR once. Blocking, timed I/O elsewhere in a
//    process using this backend has to retry on EINTR, as portable code does.

namespace http {

namespace {
// Provided buffers: this many of this size per loop (16 MiB). A buffer is
// held only from the moment data lands in it until its completion has been
// consumed, so the count bounds how many connections can complete a read in
// one round before the rest are told to try again (ENOBUFS, handled below),
// not how many connections there can be. The size bounds what one read can
// deliver: a WebSocket frame of a few tens of kilobytes in one piece.
constexpr unsigned kProvidedBuffers = 512;
constexpr size_t kProvidedBufferBytes = 32 * 1024;
constexpr unsigned kSubmissionEntries = 4096;
constexpr unsigned kCompletionEntries = 16384;

enum class OpKind : uint8_t
{
    None = 0,
    Wakeup,
    Accept,
    Recv,
    Send,
    Cancel,
    Provide
};
constexpr int kBufferGroup = 0;

// user_data: the operation's kind in the top byte, the owner's token below.
constexpr uint64_t makeUserData(OpKind kind, uint64_t token)
{
    return (static_cast<uint64_t>(kind) << 56) | (token & ((uint64_t{1} << 56) - 1));
}
constexpr OpKind kindOf(uint64_t userData)
{
    return static_cast<OpKind>(userData >> 56);
}
constexpr uint64_t tokenOf(uint64_t userData)
{
    return userData & ((uint64_t{1} << 56) - 1);
}

struct UringHandler
{
    uint64_t token = 0;
    virtual void onCompletion(OpKind kind, int result, unsigned flags, unsigned char* buffer) = 0;
    // A read that found no provided buffer free may try again.
    virtual void onBuffersAvailable() {}
    virtual ~UringHandler() = default;
};
}  // namespace

struct IoLoop::Impl
{
    Impl(size_t taskCapacity, size_t timerCapacity) : tasks(taskCapacity), timers(timerCapacity)
    {
        handlers.reserve(1024);
    }

    io_uring ring{};
    bool ringReady = false;
    bool ringDisabled = true;  // created IORING_SETUP_R_DISABLED, enabled by run()
    unsigned char* buffers = nullptr;
    int wakeupFd = -1;
    uint64_t wakeupValue = 0;
    std::atomic<bool> started{false};
    // True only while the loop thread is, or is about to be, blocked in
    // the wait: the one state in which a poster has to write the eventfd.
    std::atomic<bool> sleeping{false};
    std::thread::id loopThreadId;
    uint64_t iteration = 0;
    std::chrono::steady_clock::time_point roundStart = std::chrono::steady_clock::now();

    internal::TaskQueue tasks;
    internal::TimerQueue timers;

    // Handlers are referenced from the kernel by a token -- slot index plus
    // a generation (24 bits; the top byte of user_data is the operation
    // kind) -- so a completion for a handler that closed, whose slot may
    // since have been reused, is recognized as stale and dropped.
    struct Slot
    {
        uint32_t generation = 0;
        UringHandler* handler = nullptr;
    };
    std::vector<Slot> handlers;
    std::vector<uint32_t> freeSlots;
    // Reads that found no provided buffer free, re-armed once the round has
    // returned some.
    std::vector<uint64_t> starved;
    // Objects a callback may have released mid-dispatch while frames below
    // still refer to them, kept alive until the round has finished.
    std::vector<std::shared_ptr<void>> graveyard;

    uint64_t registerHandler(UringHandler* handler)
    {
        if (handler->token)
            return handler->token;

        uint32_t index;
        if (!freeSlots.empty()) {
            index = freeSlots.back();
            freeSlots.pop_back();
        } else {
            if (handlers.size() >= 0xFFFFFFFEu)
                throw std::runtime_error("reactor registration capacity exhausted");

            index = static_cast<uint32_t>(handlers.size());
            handlers.emplace_back();
        }
        Slot& slot = handlers[index];
        slot.generation = (slot.generation + 1) & 0xFFFFFFu;
        if (slot.generation == 0)
            slot.generation = 1;

        slot.handler = handler;
        handler->token = (static_cast<uint64_t>(slot.generation) << 32) | (index + 1u);
        return handler->token;
    }

    UringHandler* findHandler(uint64_t token)
    {
        const size_t index = static_cast<uint32_t>(token) - 1;
        if (index >= handlers.size() || handlers[index].generation != (token >> 32))
            return nullptr;

        return handlers[index].handler;
    }

    void unregisterHandler(UringHandler& handler)
    {
        if (!handler.token)
            return;

        const uint32_t index = static_cast<uint32_t>(handler.token) - 1;
        handlers[index].handler = nullptr;
        freeSlots.push_back(index);
        handler.token = 0;
    }

    // A free submission entry, flushing the queue to the kernel if it is
    // full (rare: the queue holds a round's worth).
    io_uring_sqe* sqe()
    {
        io_uring_sqe* entry = io_uring_get_sqe(&ring);
        if (!entry) {
            io_uring_submit(&ring);
            entry = io_uring_get_sqe(&ring);
            if (!entry)
                throw std::runtime_error("io_uring submission queue exhausted");
        }
        return entry;
    }

    void armWakeup()
    {
        io_uring_sqe* entry = sqe();
        io_uring_prep_read(entry, wakeupFd, &wakeupValue, sizeof(wakeupValue), 0);
        io_uring_sqe_set_data64(entry, makeUserData(OpKind::Wakeup, 0));
    }

    unsigned char* bufferAt(unsigned id) { return buffers + static_cast<size_t>(id) * kProvidedBufferBytes; }

    // Hands buffers [first, first + count) to the kernel's group. Providing
    // completes on the spot at submission and only a failure is worth a
    // completion entry.
    void provideBuffers(unsigned first, unsigned count)
    {
        io_uring_sqe* entry = sqe();
        io_uring_prep_provide_buffers(entry, bufferAt(first), static_cast<int>(kProvidedBufferBytes),
                                      static_cast<int>(count), kBufferGroup, static_cast<int>(first));
        io_uring_sqe_set_data64(entry, makeUserData(OpKind::Provide, 0));
        entry->flags |= IOSQE_CQE_SKIP_SUCCESS;
    }

    void returnBuffer(unsigned id) { provideBuffers(id, 1); }

    // A loop that is awake will see what was just queued when it next checks,
    // which it does right before sleeping; only a sleeping one needs the
    // eventfd. The check-then-sleep ordering in run() is what makes skipping
    // the write safe: `sleeping` is raised before the queue and timers are
    // consulted for the final time, so a poster either sees it raised and
    // writes, or its work is seen by that consultation.
    void wakeUp()
    {
        if (!sleeping.load())
            return;

        uint64_t one = 1;
        ssize_t n = ::write(wakeupFd, &one, sizeof(one));
        (void)n;
    }

    int computeTimeoutMilliseconds()
    {
        return timers.waitMilliseconds(internal::TimerQueue::Clock::now());
    }

    void fireExpiredTimers()
    {
        if (timers.empty())
            return;

        for (size_t fired = 0; fired < timers.capacity(); ++fired) {
            TimerCallback callback;
            if (!timers.popDue(internal::TimerQueue::Clock::now(), callback))
                break;

            try {
                callback();
            } catch (...) {
                HTTP_LOG_ERROR("Timer callback threw");
            }
        }
    }

    void drainPostedTasks()
    {
        if (tasks.hasPending())
            tasks.drain();
    }

    void handleCompletion(io_uring_cqe* cqe)
    {
        const uint64_t userData = io_uring_cqe_get_data64(cqe);
        const OpKind kind = kindOf(userData);
        if (kind == OpKind::Wakeup) {
            // A counter eventfd hands over the whole count in one read.
            armWakeup();
            return;
        }
        if (kind == OpKind::Cancel)
            return;

        if (kind == OpKind::Provide) {
            if (cqe->res < 0)
                HTTP_LOG_ERROR("io_uring provide buffers failed: %s", std::strerror(-cqe->res));

            return;
        }

        unsigned char* buffer = nullptr;
        unsigned bufferId = 0;
        if (cqe->flags & IORING_CQE_F_BUFFER) {
            bufferId = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
            buffer = bufferAt(bufferId);
        }
        if (UringHandler* handler = findHandler(tokenOf(userData)))
            handler->onCompletion(kind, cqe->res, cqe->flags, buffer);

        // Whoever the data was for has consumed it by now, or is gone.
        if (buffer)
            returnBuffer(bufferId);
    }
};

IoLoop::IoLoop(size_t taskCapacity, size_t timerCapacity)
    : impl_(std::make_unique<Impl>(taskCapacity, timerCapacity))
{
    // The loop is constructed on one thread and run on another, and with
    // SINGLE_ISSUER the kernel binds the ring to whichever task enables it:
    // created disabled here, enabled at the top of run().
    io_uring_params params{};
    params.flags = IORING_SETUP_CQSIZE | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN |
                   IORING_SETUP_R_DISABLED;
    params.cq_entries = kCompletionEntries;
    int rc = io_uring_queue_init_params(kSubmissionEntries, &impl_->ring, &params);
    if (rc < 0) {
        // An older kernel: the same ring without the newer scheduling flags.
        params = io_uring_params{};
        params.flags = IORING_SETUP_CQSIZE;
        params.cq_entries = kCompletionEntries;
        rc = io_uring_queue_init_params(kSubmissionEntries, &impl_->ring, &params);
        impl_->ringDisabled = false;
    }
    if (rc < 0)
        throw std::system_error(-rc, std::generic_category(), "io_uring_queue_init");

    impl_->ringReady = true;
    try {
        impl_->buffers = static_cast<unsigned char*>(
            std::aligned_alloc(4096, static_cast<size_t>(kProvidedBuffers) * kProvidedBufferBytes));
        if (!impl_->buffers)
            throw std::bad_alloc();

        impl_->wakeupFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (impl_->wakeupFd < 0)
            throw std::system_error(errno, std::generic_category(), "eventfd");
    } catch (...) {
        std::free(impl_->buffers);
        io_uring_queue_exit(&impl_->ring);
        throw;
    }
}

IoLoop::~IoLoop()
{
    if (impl_->wakeupFd >= 0)
        ::close(impl_->wakeupFd);

    if (impl_->ringReady)
        io_uring_queue_exit(&impl_->ring);

    std::free(impl_->buffers);
}

uint64_t IoLoop::iteration() const
{
    return impl_->iteration;
}

std::chrono::steady_clock::time_point IoLoop::now() const
{
    return impl_->roundStart;
}

void IoLoop::run()
{
    if (impl_->started.exchange(true))
        throw std::logic_error("IoLoop::run may only be called once");

    impl_->loopThreadId = std::this_thread::get_id();
    if (impl_->ringDisabled) {
        const int rc = io_uring_enable_rings(&impl_->ring);
        if (rc < 0)
            throw std::system_error(-rc, std::generic_category(), "io_uring_enable_rings");

        impl_->ringDisabled = false;
    }
    // The buffer group goes to the kernel with the first round's submission,
    // ahead of any read that could select from it: entries are processed in
    // order and providing buffers completes on the spot. (Its completion is
    // dispatched like any other -- an accept for a connection already in the
    // backlog may well complete first.)
    impl_->provideBuffers(0, kProvidedBuffers);
    impl_->armWakeup();

    while (!impl_->tasks.closed() || impl_->tasks.hasPending()) {
        if (!impl_->starved.empty()) {
            // Buffers returned during the last round go to the kernel with
            // this round's submission, ahead of these retries.
            std::vector<uint64_t> retry;
            retry.swap(impl_->starved);
            for (uint64_t token : retry) {
                if (UringHandler* handler = impl_->findHandler(token))
                    handler->onBuffersAvailable();
            }
        }
        impl_->sleeping.store(true);
        // Consulted after `sleeping` is raised -- see Impl::wakeUp.
        const bool waitless =
            impl_->tasks.hasPending() || impl_->tasks.closed() || !impl_->starved.empty();
        const int timeoutMs = waitless ? 0 : impl_->computeTimeoutMilliseconds();
        __kernel_timespec timeout{};
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_nsec = static_cast<long long>(timeoutMs % 1000) * 1000000;
        io_uring_cqe* first = nullptr;
        const int rc = io_uring_submit_and_wait_timeout(&impl_->ring, &first, waitless ? 0 : 1, &timeout,
                                                        nullptr);
        impl_->sleeping.store(false);
        ++impl_->iteration;
        impl_->roundStart = std::chrono::steady_clock::now();
        if (rc < 0 && rc != -ETIME && rc != -EINTR && rc != -EAGAIN && rc != -EBUSY) {
            HTTP_LOG_ERROR("io_uring_enter failed: %s", std::strerror(-rc));
            break;
        }
        unsigned head = 0;
        unsigned seen = 0;
        io_uring_cqe* cqe = nullptr;
        io_uring_for_each_cqe(&impl_->ring, head, cqe)
        {
            ++seen;
            impl_->handleCompletion(cqe);
        }
        io_uring_cq_advance(&impl_->ring, seen);

        impl_->fireExpiredTimers();
        impl_->drainPostedTasks();
        if (!impl_->graveyard.empty())
            impl_->graveyard.clear();
    }
    // Writes issued right before stop() -- a TLS close_notify, a last
    // response -- are still only queued here; the kernel carries them out
    // whether or not anyone reaps the completions.
    io_uring_submit(&impl_->ring);
}

void IoLoop::stop()
{
    impl_->tasks.close();
    impl_->wakeUp();
}

void IoLoop::post(std::function<void()> callback)
{
    if (!tryPost(std::move(callback)))
        throw std::runtime_error("reactor task queue is full or stopped");
}

bool IoLoop::tryPost(std::function<void()> callback)
{
    if (!impl_->tasks.tryPush(std::move(callback)))
        return false;

    impl_->wakeUp();
    return true;
}

void IoLoop::postControl(std::function<void()> callback)
{
    if (!impl_->tasks.tryPush(std::move(callback), true))
        throw std::runtime_error("reactor control queue is full or stopped");

    impl_->wakeUp();
}

size_t IoLoop::failedTasks() const
{
    return impl_->tasks.failedTasks();
}

size_t IoLoop::rejectedTasks() const
{
    return impl_->tasks.rejectedTasks();
}

size_t IoLoop::pendingTasks() const
{
    return impl_->tasks.pendingTasks();
}

uint64_t IoLoop::addTimer(std::chrono::milliseconds delay, TimerCallback callback)
{
    const uint64_t id =
        impl_->timers.tryAdd(internal::TimerQueue::after(delay), std::move(callback));
    if (!id)
        throw std::runtime_error("reactor timer capacity exhausted");

    impl_->wakeUp();
    return id;
}

void IoLoop::cancelTimer(uint64_t id)
{
    impl_->timers.cancel(id);
}

bool IoLoop::refreshTimer(uint64_t id, std::chrono::milliseconds delay)
{
    if (!impl_->timers.refresh(id, internal::TimerQueue::after(delay)))
        return false;

    impl_->wakeUp();
    return true;
}

// ---------------------------------------------------------------------
// TcpConnection
// ---------------------------------------------------------------------

struct TcpConnection::Impl : UringHandler
{
    IoLoop& loop;
    socket_t fd;
    bool closed = false;

    unsigned char* readBuf = nullptr;
    size_t readCap = 0;
    IoCallback readCallback;
    bool readPending = false;    // a caller is waiting
    bool recvSubmitted = false;  // and a recv is with the kernel for it

    const unsigned char* writeBuf = nullptr;
    size_t writeLen = 0;
    size_t writeOffset = 0;
    IoCallback writeCallback;
    bool writePending = false;
    bool sendSubmitted = false;

    explicit Impl(IoLoop& l, socket_t f) : loop(l), fd(f) {}

    void submitRecv()
    {
        io_uring_sqe* entry = loop.impl()->sqe();
        if (readBuf) {
            io_uring_prep_recv(entry, fd, readBuf, readCap, 0);
        } else {
            io_uring_prep_recv(entry, fd, nullptr, 0, 0);
            entry->flags |= IOSQE_BUFFER_SELECT;
            entry->buf_group = kBufferGroup;
        }
        // A read is issued right after answering, before the peer can have
        // replied: poll for data first instead of attempting a recv that
        // would only fail (and, with a selected buffer, take and release one).
        entry->ioprio |= IORING_RECVSEND_POLL_FIRST;
        io_uring_sqe_set_data64(entry, makeUserData(OpKind::Recv, token));
        recvSubmitted = true;
    }

    void onBuffersAvailable() override
    {
        if (!closed && readPending && !recvSubmitted)
            submitRecv();
    }

    void submitSend()
    {
        io_uring_sqe* entry = loop.impl()->sqe();
        io_uring_prep_send(entry, fd, writeBuf + writeOffset, writeLen - writeOffset, MSG_NOSIGNAL);
        io_uring_sqe_set_data64(entry, makeUserData(OpKind::Send, token));
        sendSubmitted = true;
    }

    void completeRead(IoResult result)
    {
        readPending = false;
        auto callback = std::move(readCallback);
        readCallback = nullptr;
        callback(result);
    }

    void completeWrite(IoResult result)
    {
        writePending = false;
        auto callback = std::move(writeCallback);
        writeCallback = nullptr;
        callback(result);
    }

    void onCompletion(OpKind kind, int result, unsigned flags, unsigned char* buffer) override
    {
        (void)flags;
        if (kind == OpKind::Recv) {
            recvSubmitted = false;
            if (closed || !readPending)
                return;

            if (result == -ENOBUFS) {
                // Every provided buffer was in use when the data arrived; the
                // loop re-arms this read once some have come back.
                loop.impl()->starved.push_back(token);
                return;
            }
            if (result < 0) {
                completeRead(IoResult{0, false, -result, nullptr});
                return;
            }
            completeRead(IoResult{static_cast<size_t>(result), true, 0, readBuf ? readBuf : buffer});
            return;
        }
        if (kind == OpKind::Send) {
            sendSubmitted = false;
            if (closed || !writePending)
                return;

            if (result < 0) {
                completeWrite(IoResult{writeOffset, false, -result, nullptr});
                return;
            }
            writeOffset += static_cast<size_t>(result);
            if (writeOffset < writeLen) {
                submitSend();
                return;
            }
            completeWrite(IoResult{writeLen, true, 0, nullptr});
        }
    }
};

TcpConnection::TcpConnection(IoLoop& loop, socket_t fd)
    : loop_(loop), fd_(fd), impl_(std::make_shared<Impl>(loop, fd))
{
    setNonBlocking(fd_);
    setTcpNoDelay(fd_);
    loop_.impl()->registerHandler(impl_.get());
}

TcpConnection::~TcpConnection()
{
    close();
}

void TcpConnection::asyncRead(unsigned char* buffer, size_t bufferCapacity, IoCallback callback)
{
    Impl& impl = *impl_;
    if (impl.readPending)
        throw std::logic_error("read already pending");

    if (impl.closed) {
        callback(IoResult{0, false, EBADF, nullptr});
        return;
    }
    impl.readBuf = buffer;
    impl.readCap = bufferCapacity;
    impl.readCallback = std::move(callback);
    impl.readPending = true;
    impl.submitRecv();
}

void TcpConnection::asyncWrite(const unsigned char* data, size_t length, IoCallback callback)
{
    Impl& impl = *impl_;
    if (impl.writePending)
        throw std::logic_error("write already pending");

    if (impl.closed) {
        callback(IoResult{0, false, EBADF, nullptr});
        return;
    }
    if (length == 0) {
        callback(IoResult{0, true, 0, nullptr});
        return;
    }
    impl.writeBuf = data;
    impl.writeLen = length;
    impl.writeOffset = 0;
    impl.writeCallback = std::move(callback);
    impl.writePending = true;
    impl.submitSend();
}

size_t TcpConnection::tryWrite(const unsigned char* head, size_t headLength,
                               const unsigned char* body, size_t bodyLength)
{
    Impl& impl = *impl_;
    if (impl.writePending || impl.closed)
        return 0;

    // A synchronous send: one syscall, but for a payload large enough to
    // call this it is cheaper than copying the payload into a buffer that
    // lives until a later round.
    iovec parts[2];
    parts[0].iov_base = const_cast<unsigned char*>(head);
    parts[0].iov_len = headLength;
    parts[1].iov_base = const_cast<unsigned char*>(body);
    parts[1].iov_len = bodyLength;
    msghdr message{};
    message.msg_iov = parts;
    message.msg_iovlen = 2;
    ssize_t n;
    do {
        n = ::sendmsg(fd_, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
    } while (n < 0 && errno == EINTR);
    return n < 0 ? 0 : static_cast<size_t>(n);
}

void TcpConnection::close()
{
    Impl& impl = *impl_;
    if (impl.closed && fd_ == kInvalidSocket)
        return;

    // A completion callback below may drop the last reference to this
    // connection while Impl frames are still on the stack; the loop keeps
    // the Impl until the round is over.
    IoLoop::Impl& loop = *loop_.impl();
    loop.graveyard.push_back(impl_);
    impl.closed = true;
    // Operations still with the kernel hold the socket open until they
    // finish: shut it down so they do, and cancel them outright. They are
    // cancelled by their user_data, never by descriptor -- the number may
    // belong to a new connection by the time the cancel runs.
    ::shutdown(fd_, SHUT_RDWR);
    if (impl.token && loop.ringReady) {
        for (OpKind kind : {OpKind::Recv, OpKind::Send}) {
            const bool submitted = kind == OpKind::Recv ? impl.recvSubmitted : impl.sendSubmitted;
            if (!submitted)
                continue;

            io_uring_sqe* entry = loop.sqe();
            io_uring_prep_cancel64(entry, makeUserData(kind, impl.token), IORING_ASYNC_CANCEL_ALL);
            io_uring_sqe_set_data64(entry, makeUserData(OpKind::Cancel, 0));
        }
    }
    loop.unregisterHandler(impl);
    closeSocket(fd_);
    fd_ = kInvalidSocket;

    // Fire any still-pending completions so callbacks holding a reference
    // to the owning session actually release it -- otherwise closing
    // mid-operation would leak the session forever.
    if (impl.readPending)
        impl.completeRead(IoResult{0, false, ECANCELED, nullptr});

    if (impl.writePending)
        impl.completeWrite(IoResult{impl.writeOffset, false, ECANCELED, nullptr});
}

bool TcpConnection::isOpen() const
{
    return !impl_->closed && fd_ != kInvalidSocket;
}

const std::string& TcpConnection::peerAddress() const
{
    if (peerResolved_)
        return peerAddress_;

    peerResolved_ = true;
    sockaddr_storage addr{};
    socklen_t len = sizeof(addr);
    if (fd_ == kInvalidSocket ||
        getpeername(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        return peerAddress_;

    char host[NI_MAXHOST] = {0};
    char port[NI_MAXSERV] = {0};
    if (getnameinfo(reinterpret_cast<sockaddr*>(&addr), len, host, sizeof(host), port, sizeof(port),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return peerAddress_;
    }
    peerAddress_.reserve(std::strlen(host) + 1 + std::strlen(port));
    peerAddress_ = host;
    peerAddress_ += ':';
    peerAddress_ += port;
    return peerAddress_;
}

// ---------------------------------------------------------------------
// TcpListener
// ---------------------------------------------------------------------

struct TcpListener::Impl : UringHandler
{
    IoLoop& loop;
    socket_t fd = kInvalidSocket;
    bool accepting = false;
    AcceptCallback onAccept;

    explicit Impl(IoLoop& l) : loop(l) {}

    void submitAccept()
    {
        io_uring_sqe* entry = loop.impl()->sqe();
        io_uring_prep_multishot_accept(entry, fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        io_uring_sqe_set_data64(entry, makeUserData(OpKind::Accept, token));
        accepting = true;
    }

    void onCompletion(OpKind kind, int result, unsigned flags, unsigned char*) override
    {
        if (kind != OpKind::Accept || fd == kInvalidSocket)
            return;

        // A multishot accept keeps delivering until it says otherwise.
        if (!(flags & IORING_CQE_F_MORE))
            accepting = false;

        if (result >= 0) {
            if (onAccept)
                onAccept(result);
        } else if (result != -ECANCELED && !wouldBlock(-result)) {
            HTTP_LOG_WARN("accept() failed: %s", std::strerror(-result));
        }
        if (!accepting && fd != kInvalidSocket)
            submitAccept();
    }
};

TcpListener::TcpListener(IoLoop& loop, const std::string& host, uint16_t port, bool reusePort)
    : loop_(loop), impl_(std::make_shared<Impl>(loop))
{
    ListenAddress address;
    if (!resolveListenAddress(host, port, address))
        throw std::invalid_argument("invalid listen address");

    impl_->fd = ::socket(address.family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (impl_->fd == kInvalidSocket)
        throw std::system_error(errno, std::generic_category(), "socket");

    setReuseAddr(impl_->fd);
    if (reusePort)
        setReusePort(impl_->fd);

    if (address.family == AF_INET6) {
        const int v6Only = address.dualStack ? 0 : 1;
        setsockopt(impl_->fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6Only, sizeof(v6Only));
    }
    setNonBlocking(impl_->fd);

    if (::bind(impl_->fd, reinterpret_cast<sockaddr*>(&address.storage), address.length) != 0) {
        int error = errno;
        close();
        throw std::system_error(error, std::generic_category(), "bind");
    }
    if (::listen(impl_->fd, SOMAXCONN) != 0) {
        int error = errno;
        close();
        throw std::system_error(error, std::generic_category(), "listen");
    }
}

TcpListener::~TcpListener()
{
    close();
}

void TcpListener::asyncAccept(AcceptCallback onAccept)
{
    impl_->onAccept = std::move(onAccept);
    loop_.impl()->registerHandler(impl_.get());
    impl_->submitAccept();
}

void TcpListener::close()
{
    IoLoop::Impl& loop = *loop_.impl();
    if (impl_->token && impl_->accepting && loop.ringReady) {
        io_uring_sqe* entry = loop.sqe();
        io_uring_prep_cancel64(entry, makeUserData(OpKind::Accept, impl_->token), IORING_ASYNC_CANCEL_ALL);
        io_uring_sqe_set_data64(entry, makeUserData(OpKind::Cancel, 0));
        impl_->accepting = false;
    }
    loop.unregisterHandler(*impl_);
    if (impl_->fd == kInvalidSocket)
        return;

    closeSocket(impl_->fd);
    impl_->fd = kInvalidSocket;
}

}  // namespace http
