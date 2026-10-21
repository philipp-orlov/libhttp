// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/io_loop.hpp"
#include "http/internal/task_queue.hpp"
#include "http/internal/timer_queue.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <unistd.h>
#include <netdb.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

#include "http/logger.hpp"

// Linux backend, built on edge-triggered epoll.
//
// Design notes and the tradeoffs behind them:
//  - A connection is registered with epoll exactly once, for EPOLLIN |
//    EPOLLET | EPOLLRDHUP, when it is created, and deregistered when it
//    closes. EPOLLOUT is added only while a write is actually blocked and
//    removed as soon as it completes. That is what keeps epoll_ctl off the
//    per-request path: a keep-alive connection answering a million requests
//    costs two epoll_ctl calls in total, not two per request. (Level-
//    triggered would need the interest set toggled around every operation,
//    or it would spin on a readable socket nobody is reading from -- e.g.
//    while a handler that answers later is still running.)
//  - Readiness is tracked in two flags. `readable` is cleared when recv()
//    reports EAGAIN or returns fewer bytes than asked for (TCP hands over
//    everything queued, so a short read means the queue was empty at that
//    instant); any byte arriving afterwards raises a fresh edge. A read
//    issued while it is clear simply waits for that edge instead of paying
//    for a recv() that would only say EAGAIN -- which, after answering a
//    request, is exactly what the next read on a healthy connection would
//    otherwise do. `writable` works the same way for send().
//  - AsyncRead/AsyncWrite try the syscall immediately (optimistic I/O)
//    when the flag allows, since most reads and writes on a healthy
//    connection complete at once. To keep call stacks bounded on a busy
//    pipelined keep-alive connection (where a completion callback
//    immediately issues the next operation, which can itself complete
//    immediately, ad infinitum), a per-thread synchronous-nesting counter
//    caps how many optimistic completions can chain; past it the operation
//    is queued on the loop and attempted from the loop's top level after the
//    current batch of events, which resets the stack.

namespace http {

namespace {
constexpr int kMaxSyncDepth = 16;
constexpr uint32_t kConnectionInterest = EPOLLIN | EPOLLET | EPOLLRDHUP;
// The loop's receive buffer for reads issued without a buffer of their own.
// Each recv() into it completes -- and is consumed by its callback -- before
// the next can happen, so one serves every connection of the loop. Large
// enough for a burst of pipelined requests or a WebSocket frame of tens of
// kilobytes to arrive in one recv().
constexpr size_t kReceiveBufferBytes = 256 * 1024;
thread_local int t_syncDepth = 0;

struct EpollHandler
{
    uint64_t token = 0;
    virtual void onEvent(uint32_t events) = 0;
    // Runs from the loop's top level for an operation that was deferred
    // because the synchronous nesting limit had been reached.
    virtual void onDeferred() = 0;
    virtual ~EpollHandler() = default;
};

struct SyncDepth
{
    SyncDepth() { ++t_syncDepth; }
    ~SyncDepth() { --t_syncDepth; }
};
}  // namespace

struct IoLoop::Impl
{
    Impl(size_t taskCapacity, size_t timerCapacity) : tasks(taskCapacity), timers(timerCapacity)
    {
        handlers.reserve(1024);
    }
    int epfd = -1;
    int wakeupFd = -1;
    std::atomic<bool> started{false};
    // True only while the loop thread is, or is about to be, blocked in
    // epoll_wait: the one state in which a poster has to write the eventfd.
    std::atomic<bool> sleeping{false};
    std::thread::id loopThreadId;
    uint64_t iteration = 0;
    std::chrono::steady_clock::time_point roundStart = std::chrono::steady_clock::now();

    internal::TaskQueue tasks;
    internal::TimerQueue timers;

    // Handlers are referenced from epoll by a token -- slot index plus a
    // generation -- rather than by pointer, so an event already fetched for a
    // handler that closed (and whose slot may since have been reused) is
    // recognized as stale and dropped. Everything here runs on the loop
    // thread, so the slots hold plain pointers.
    struct EventSlot
    {
        uint32_t generation = 0;
        EpollHandler* handler = nullptr;
    };
    std::vector<EventSlot> handlers;
    std::vector<uint32_t> freeSlots;
    // Operations that hit the synchronous nesting limit, run from run()'s
    // top level after the current event batch.
    std::vector<uint64_t> deferred;
    // Objects a callback may have released mid-dispatch while frames below
    // still refer to them (a connection's Impl closed from inside its own
    // completion callback, say), kept alive until the round has finished.
    std::vector<std::shared_ptr<void>> graveyard;
    std::unique_ptr<unsigned char[]> receiveBuffer{new unsigned char[kReceiveBufferBytes]};

    uint64_t registerHandler(EpollHandler* handler)
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
        EventSlot& slot = handlers[index];
        if (++slot.generation == 0)
            ++slot.generation;

        slot.handler = handler;
        handler->token = (static_cast<uint64_t>(slot.generation) << 32) | (index + 1u);
        return handler->token;
    }

    EpollHandler* findHandler(uint64_t token)
    {
        const size_t index = static_cast<uint32_t>(token) - 1;
        if (index >= handlers.size() || handlers[index].generation != (token >> 32))
            return nullptr;

        return handlers[index].handler;
    }

    void unregisterHandler(EpollHandler& handler)
    {
        if (!handler.token)
            return;

        const uint32_t index = static_cast<uint32_t>(handler.token) - 1;
        handlers[index].handler = nullptr;
        freeSlots.push_back(index);
        handler.token = 0;
    }

    void defer(EpollHandler& handler)
    {
        deferred.push_back(handler.token);
    }

    void runDeferred()
    {
        // Handlers may defer themselves again while this runs; those wait
        // for the next round.
        std::vector<uint64_t> batch;
        batch.swap(deferred);
        for (uint64_t token : batch) {
            if (EpollHandler* handler = findHandler(token))
                handler->onDeferred();
        }
        if (deferred.empty()) {
            batch.clear();
            deferred.swap(batch);  // keep the capacity
        }
    }

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
};

IoLoop::IoLoop(size_t taskCapacity, size_t timerCapacity)
    : impl_(std::make_unique<Impl>(taskCapacity, timerCapacity))
{
    impl_->epfd = epoll_create1(EPOLL_CLOEXEC);
    impl_->wakeupFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (impl_->epfd < 0 || impl_->wakeupFd < 0) {
        int error = errno;
        if (impl_->epfd >= 0)
            ::close(impl_->epfd);

        if (impl_->wakeupFd >= 0)
            ::close(impl_->wakeupFd);

        throw std::system_error(error, std::generic_category(), "reactor creation");
    }
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u64 = 0;
    epoll_ctl(impl_->epfd, EPOLL_CTL_ADD, impl_->wakeupFd, &ev);
}

IoLoop::~IoLoop()
{
    if (impl_->wakeupFd >= 0)
        ::close(impl_->wakeupFd);

    if (impl_->epfd >= 0)
        ::close(impl_->epfd);
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
    std::vector<epoll_event> events(1024);

    while (!impl_->tasks.closed() || impl_->tasks.hasPending()) {
        impl_->sleeping.store(true);
        // Consulted after `sleeping` is raised -- see Impl::wakeUp.
        const int timeoutMs =
            !impl_->deferred.empty() || impl_->tasks.hasPending() || impl_->tasks.closed()
                ? 0
                : impl_->computeTimeoutMilliseconds();
        int n = epoll_wait(impl_->epfd, events.data(), static_cast<int>(events.size()), timeoutMs);
        impl_->sleeping.store(false);
        ++impl_->iteration;
        impl_->roundStart = std::chrono::steady_clock::now();
        if (n < 0) {
            if (errno == EINTR)
                continue;

            HTTP_LOG_ERROR("epoll_wait failed: %s", std::strerror(errno));
            break;
        }
        for (int i = 0; i < n; ++i) {
            epoll_event& ev = events[i];
            if (ev.data.u64 == 0) {
                // A counter eventfd hands over the whole count and resets in
                // one read; a second read would only ever return EAGAIN.
                uint64_t val;
                ssize_t drained = ::read(impl_->wakeupFd, &val, sizeof(val));
                (void)drained;
                continue;
            }
            if (EpollHandler* handler = impl_->findHandler(ev.data.u64))
                handler->onEvent(ev.events);
        }
        if (!impl_->deferred.empty())
            impl_->runDeferred();

        impl_->fireExpiredTimers();
        impl_->drainPostedTasks();
        if (!impl_->graveyard.empty())
            impl_->graveyard.clear();
    }
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

struct TcpConnection::Impl : EpollHandler
{
    IoLoop& loop;
    socket_t fd;
    bool registered = false;   // in epoll's interest set (kConnectionInterest)
    bool writeInterest = false;  // EPOLLOUT currently added to it
    bool closed = false;
    // Believed-ready flags; see the design notes at the top of the file.
    bool readable = true;
    bool writable = true;
    bool deferred = false;

    unsigned char* readBuf = nullptr;
    size_t readCap = 0;
    IoCallback readCallback;
    bool readPending = false;

    const unsigned char* writeBuf = nullptr;
    size_t writeLen = 0;
    size_t writeOffset = 0;
    IoCallback writeCallback;
    bool writePending = false;

    explicit Impl(IoLoop& l, socket_t f) : loop(l), fd(f) {}

    void registerWithLoop()
    {
        epoll_event ev{};
        ev.events = kConnectionInterest;
        ev.data.u64 = loop.impl()->registerHandler(this);
        if (epoll_ctl(loop.impl()->epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
            const int error = errno;
            loop.impl()->unregisterHandler(*this);
            throw std::system_error(error, std::generic_category(), "epoll_ctl");
        }
        registered = true;
    }

    void setWriteInterest(bool on)
    {
        if (on == writeInterest || !registered)
            return;

        epoll_event ev{};
        ev.events = kConnectionInterest | (on ? EPOLLOUT : 0u);
        ev.data.u64 = token;
        if (epoll_ctl(loop.impl()->epfd, EPOLL_CTL_MOD, fd, &ev) == 0)
            writeInterest = on;
    }

    void attemptRead()
    {
        if (!readPending || closed || !readable)
            return;

        unsigned char* target = readBuf ? readBuf : loop.impl()->receiveBuffer.get();
        const size_t capacity = readBuf ? readCap : kReceiveBufferBytes;
        ssize_t n;
        do {
            n = ::recv(fd, target, capacity, 0);
        } while (n < 0 && errno == EINTR);
        if (n >= 0) {
            if (static_cast<size_t>(n) < capacity)
                readable = false;

            readPending = false;
            auto callback = std::move(readCallback);
            readCallback = nullptr;
            SyncDepth depth;
            callback(IoResult{static_cast<size_t>(n), true, 0, target});
        } else {
            int err = errno;
            if (wouldBlock(err)) {
                readable = false;
            } else {
                readPending = false;
                auto callback = std::move(readCallback);
                readCallback = nullptr;
                SyncDepth depth;
                callback(IoResult{0, false, err});
            }
        }
    }

    void attemptWrite()
    {
        if (!writePending || closed || !writable)
            return;

        ssize_t n;
        do {
            n = ::send(fd, writeBuf + writeOffset, writeLen - writeOffset,
#ifdef MSG_NOSIGNAL
                       MSG_NOSIGNAL
#else
                       0
#endif
            );
        } while (n < 0 && errno == EINTR);
        if (n >= 0) {
            writeOffset += static_cast<size_t>(n);
            if (writeOffset >= writeLen) {
                writePending = false;
                setWriteInterest(false);
                auto callback = std::move(writeCallback);
                writeCallback = nullptr;
                SyncDepth depth;
                callback(IoResult{writeLen, true, 0});
            } else {
                // A short send means the socket buffer is full.
                writable = false;
                setWriteInterest(true);
            }
        } else {
            int err = errno;
            if (wouldBlock(err)) {
                writable = false;
                setWriteInterest(true);
            } else {
                writePending = false;
                setWriteInterest(false);
                auto callback = std::move(writeCallback);
                writeCallback = nullptr;
                SyncDepth depth;
                callback(IoResult{writeOffset, false, err});
            }
        }
    }

    void onEvent(uint32_t events) override
    {
        // Errors and hangups are surfaced by the syscalls themselves: a read
        // returns 0 (orderly EOF) or fails, a write fails with EPIPE or
        // ECONNRESET. Marking both directions ready is all that is needed
        // for a pending operation to run into that.
        if (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            readable = true;

        if (events & (EPOLLOUT | EPOLLHUP | EPOLLERR))
            writable = true;

        if (readPending)
            attemptRead();

        if (writePending && !closed)
            attemptWrite();
    }

    void onDeferred() override
    {
        deferred = false;
        attemptRead();
        if (!closed)
            attemptWrite();
    }

    // Past the nesting limit, the attempt runs from the loop's top level
    // instead of here.
    void attemptOrDefer(void (Impl::*attempt)())
    {
        if (t_syncDepth < kMaxSyncDepth) {
            (this->*attempt)();
            return;
        }
        if (!deferred) {
            deferred = true;
            loop.impl()->defer(*this);
        }
    }
};

TcpConnection::TcpConnection(IoLoop& loop, socket_t fd)
    : loop_(loop), fd_(fd), impl_(std::make_shared<Impl>(loop, fd))
{
    setNonBlocking(fd_);
    setTcpNoDelay(fd_);
    impl_->registerWithLoop();
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
        callback(IoResult{0, false, EBADF});
        return;
    }
    impl.readBuf = buffer;
    impl.readCap = bufferCapacity;
    impl.readCallback = std::move(callback);
    impl.readPending = true;
    impl.attemptOrDefer(&Impl::attemptRead);
}

void TcpConnection::asyncWrite(const unsigned char* data, size_t length, IoCallback callback)
{
    Impl& impl = *impl_;
    if (impl.writePending)
        throw std::logic_error("write already pending");

    if (impl.closed) {
        callback(IoResult{0, false, EBADF});
        return;
    }
    if (length == 0) {
        callback(IoResult{0, true, 0});
        return;
    }
    impl.writeBuf = data;
    impl.writeLen = length;
    impl.writeOffset = 0;
    impl.writeCallback = std::move(callback);
    impl.writePending = true;
    impl.attemptOrDefer(&Impl::attemptWrite);
}

size_t TcpConnection::tryWrite(const unsigned char* head, size_t headLength,
                               const unsigned char* body, size_t bodyLength)
{
    Impl& impl = *impl_;
    if (impl.writePending || impl.closed || !impl.writable)
        return 0;

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
        n = ::sendmsg(fd_, &message,
#ifdef MSG_NOSIGNAL
                      MSG_NOSIGNAL
#else
                      0
#endif
        );
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        // Any other failure is left for the asyncWrite that follows to
        // report through its callback.
        if (wouldBlock(errno)) {
            impl.writable = false;
            impl.setWriteInterest(true);
        }
        return 0;
    }
    if (static_cast<size_t>(n) < headLength + bodyLength) {
        impl.writable = false;
        impl.setWriteInterest(true);
    }
    return static_cast<size_t>(n);
}

void TcpConnection::close()
{
    Impl& impl = *impl_;
    if (impl.closed && fd_ == kInvalidSocket)
        return;

    // A completion callback below may drop the last reference to this
    // connection while Impl frames are still on the stack; the loop keeps
    // the Impl until the round is over.
    loop_.impl()->graveyard.push_back(impl_);
    if (impl.registered) {
        epoll_ctl(loop_.impl()->epfd, EPOLL_CTL_DEL, fd_, nullptr);
        impl.registered = false;
        impl.writeInterest = false;
    }
    impl.closed = true;
    loop_.impl()->unregisterHandler(impl);
    closeSocket(fd_);
    fd_ = kInvalidSocket;

    // Fire any still-pending completions so callbacks holding a reference
    // to the owning session actually release it -- otherwise closing
    // mid-operation would leak the session forever.
    if (impl.readPending) {
        impl.readPending = false;
        auto callback = std::move(impl.readCallback);
        impl.readCallback = nullptr;
        if (callback)
            callback(IoResult{0, false, ECANCELED});
    }
    if (impl.writePending) {
        impl.writePending = false;
        auto callback = std::move(impl.writeCallback);
        impl.writeCallback = nullptr;
        if (callback)
            callback(IoResult{impl.writeOffset, false, ECANCELED});
    }
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

struct TcpListener::Impl : EpollHandler
{
    IoLoop& loop;
    socket_t fd = kInvalidSocket;
    bool epollAdded = false;
    AcceptCallback onAccept;
    // Held open so that, when the process is out of descriptors, one can be
    // freed to accept-and-close the queued connection -- otherwise a level-
    // triggered listener stays readable and the loop spins on the failure.
    int spareFd = -1;
    std::chrono::steady_clock::time_point lastFdWarning{};

    explicit Impl(IoLoop& l) : loop(l) { spareFd = ::open("/dev/null", O_RDONLY | O_CLOEXEC); }
    ~Impl() override
    {
        if (spareFd >= 0)
            ::close(spareFd);
    }

    // Level-triggered: a burst larger than one batch is simply reported
    // again on the next wait.
    void onEvent(uint32_t) override
    {
        for (size_t accepted = 0; accepted < 64 && fd != kInvalidSocket; ++accepted) {
            socket_t client = ::accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (client == kInvalidSocket) {
                if (errno == EINTR)
                    continue;

                if (errno == EMFILE || errno == ENFILE) {
                    shedOne();
                    continue;
                }
                if (!wouldBlock(errno)) {
                    HTTP_LOG_WARN("accept() failed: %s", std::strerror(errno));
                }
                return;
            }
            if (onAccept)
                onAccept(client);
        }
    }

    void shedOne()
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastFdWarning > std::chrono::seconds(1)) {
            lastFdWarning = now;
            HTTP_LOG_WARN("accept(): out of file descriptors, turning connections away");
        }
        if (spareFd < 0)
            return;

        ::close(spareFd);
        const int client = ::accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client >= 0)
            ::close(client);

        spareFd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    }
    void onDeferred() override {}
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
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u64 = loop_.impl()->registerHandler(impl_.get());
    if (epoll_ctl(loop_.impl()->epfd, EPOLL_CTL_ADD, impl_->fd, &ev) != 0)
        throw std::system_error(errno, std::generic_category(), "listen registration");

    impl_->epollAdded = true;
}

void TcpListener::close()
{
    loop_.impl()->unregisterHandler(*impl_);
    if (impl_->fd == kInvalidSocket)
        return;

    if (impl_->epollAdded) {
        epoll_ctl(loop_.impl()->epfd, EPOLL_CTL_DEL, impl_->fd, nullptr);
        impl_->epollAdded = false;
    }
    closeSocket(impl_->fd);
    impl_->fd = kInvalidSocket;
}

}  // namespace http
