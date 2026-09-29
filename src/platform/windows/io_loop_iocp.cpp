// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/io_loop.hpp"
#include "http/internal/task_queue.hpp"
#include "http/internal/timer_queue.hpp"

// Windows backend, built on IOCP -- the native completion-port proactor,
// so this mirrors the epoll backend's *behavior* exactly (same public
// API, same "callbacks always fire from the loop, never synchronously
// nested" contract) while using genuinely async OS calls (AcceptEx /
// WSARecv / WSASend with OVERLAPPED) instead of emulating readiness.
//
// NOTE: this file is only ever compiled on Windows (see CMakeLists.txt);
// it has been written to the standard IOCP patterns but has not been
// build- or run-tested in this environment (no Windows toolchain
// available here). Please validate it on an actual Windows target before
// relying on it in production.

#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "http/logger.hpp"

namespace http {

namespace {

constexpr ULONG_PTR kWakeupKey = 0;
constexpr ULONG_PTR kIoKey =
    1;  // per-handler completion key; handler ptr carried via OVERLAPPED subtype instead

LPFN_ACCEPTEX g_AcceptEx = nullptr;
LPFN_GETACCEPTEXSOCKADDRS g_GetAcceptExSockaddrs = nullptr;

void ensureExtensionFunctionsLoaded(socket_t anySocket)
{
    if (g_AcceptEx && g_GetAcceptExSockaddrs)
        return;

    GUID guidAcceptEx = WSAID_ACCEPTEX;
    GUID guidGetAcceptExSockaddrs = WSAID_GETACCEPTEXSOCKADDRS;
    DWORD bytes = 0;
    WSAIoctl(anySocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &guidAcceptEx, sizeof(guidAcceptEx),
             &g_AcceptEx, sizeof(g_AcceptEx), &bytes, nullptr, nullptr);
    WSAIoctl(anySocket, SIO_GET_EXTENSION_FUNCTION_POINTER, &guidGetAcceptExSockaddrs,
             sizeof(guidGetAcceptExSockaddrs), &g_GetAcceptExSockaddrs,
             sizeof(g_GetAcceptExSockaddrs), &bytes, nullptr, nullptr);
}

enum class OpKind
{
    Read,
    Write,
    Accept,
    Wakeup
};

struct IocpOp : OVERLAPPED
{
    OpKind kind;
};

// Base for anything that can own pending IOCP operations; the handler
// pointer travels as the OVERLAPPED's containing object is looked up by
// downcasting from the IocpOp -- each concrete Impl below embeds its ops
// and implements OnComplete itself, dispatched from IoLoop::run().
struct IocpHandler
{
    virtual void onComplete(IocpOp* op, DWORD bytesTransferred, bool ok, int errorCode) = 0;
    virtual ~IocpHandler() = default;
};

}  // namespace

struct IoLoop::Impl
{
    Impl(size_t taskCapacity, size_t timerCapacity) : tasks(taskCapacity), timers(timerCapacity) {}
    HANDLE iocp = nullptr;
    std::atomic<bool> started{false};
    std::thread::id loopThreadId;
    uint64_t iteration = 0;
    std::chrono::steady_clock::time_point roundStart = std::chrono::steady_clock::now();

    internal::TaskQueue tasks;

    internal::TimerQueue timers;

    void wakeUp() { PostQueuedCompletionStatus(iocp, 0, kWakeupKey, nullptr); }

    DWORD computeTimeoutMilliseconds()
    {
        return static_cast<DWORD>(timers.waitMilliseconds(internal::TimerQueue::Clock::now()));
    }

    void fireExpiredTimers()
    {
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

    void drainPostedTasks() { tasks.drain(); }
};

IoLoop::IoLoop(size_t taskCapacity, size_t timerCapacity)
    : impl_(std::make_unique<Impl>(taskCapacity, timerCapacity))
{
    impl_->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
}

IoLoop::~IoLoop()
{
    if (impl_->iocp)
        CloseHandle(impl_->iocp);
}

void IoLoop::run()
{
    if (impl_->started.exchange(true))
        throw std::logic_error("IoLoop::run may only be called once");

    impl_->loopThreadId = std::this_thread::get_id();

    while (!impl_->tasks.closed() || impl_->tasks.hasPending()) {
        DWORD timeoutMs = impl_->computeTimeoutMilliseconds();
        DWORD bytesTransferred = 0;
        ULONG_PTR completionKey = 0;
        OVERLAPPED* overlapped = nullptr;

        BOOL ok = GetQueuedCompletionStatus(impl_->iocp, &bytesTransferred, &completionKey,
                                            &overlapped, timeoutMs);
        ++impl_->iteration;
        impl_->roundStart = std::chrono::steady_clock::now();
        if (!ok && overlapped == nullptr) {
            // Timeout (no completion within timeoutMs) -- fall through to
            // process timers/tasks below.
        } else if (overlapped != nullptr) {
            int errorCode = ok ? 0 : static_cast<int>(GetLastError());
            auto* op = static_cast<IocpOp*>(overlapped);
            if (op->kind == OpKind::Wakeup) {
                // no-op marker; nothing to do besides looping around
            } else {
                auto* handler = reinterpret_cast<IocpHandler*>(completionKey);
                handler->onComplete(op, bytesTransferred, ok != 0, errorCode);
            }
        }
        // completionKey == kWakeupKey with overlapped == nullptr is the
        // explicit wakeUp() ping; nothing to do besides looping around.

        impl_->fireExpiredTimers();
        impl_->drainPostedTasks();
    }
}

uint64_t IoLoop::iteration() const
{
    return impl_->iteration;
}

std::chrono::steady_clock::time_point IoLoop::now() const
{
    return impl_->roundStart;
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

struct TcpConnection::Impl : IocpHandler
{
    IoLoop& loop;
    socket_t fd;
    bool closed = false;
    bool associated = false;

    IocpOp readOp{};
    WSABUF readWsaBuf{};
    IoCallback readCallback;
    // For reads issued without a buffer of their own (see IoLoop's
    // asyncRead): IOCP fills buffers asynchronously, so each connection
    // needs one of its own, made on first use.
    std::vector<unsigned char> ownReceive;

    IocpOp writeOp{};
    WSABUF writeWsaBuf{};
    const unsigned char* writeBuf = nullptr;
    size_t writeLen = 0;
    size_t writeOffset = 0;
    IoCallback writeCallback;

    explicit Impl(IoLoop& l, socket_t f) : loop(l), fd(f)
    {
        std::memset(&readOp, 0, sizeof(readOp));
        std::memset(&writeOp, 0, sizeof(writeOp));
        readOp.kind = OpKind::Read;
        writeOp.kind = OpKind::Write;
    }

    void ensureAssociated()
    {
        if (associated)
            return;

        CreateIoCompletionPort(reinterpret_cast<HANDLE>(fd), loop.impl()->iocp,
                               reinterpret_cast<ULONG_PTR>(this), 0);
        associated = true;
    }

    void submitRead()
    {
        readWsaBuf.buf = reinterpret_cast<CHAR*>(pendingReadBuf);
        readWsaBuf.len = static_cast<ULONG>(pendingReadCap);
        DWORD flags = 0;
        std::memset(&readOp, 0, sizeof(WSAOVERLAPPED));
        readOp.kind = OpKind::Read;
        int rc = WSARecv(fd, &readWsaBuf, 1, nullptr, &flags, &readOp, nullptr);
        if (rc == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err != WSA_IO_PENDING) {
                auto callback = std::move(readCallback);
                readCallback = nullptr;
                if (callback)
                    callback(IoResult{0, false, err});
            }
        }
    }

    void submitWrite()
    {
        writeWsaBuf.buf = const_cast<CHAR*>(reinterpret_cast<const CHAR*>(writeBuf + writeOffset));
        writeWsaBuf.len = static_cast<ULONG>(writeLen - writeOffset);
        std::memset(&writeOp, 0, sizeof(WSAOVERLAPPED));
        writeOp.kind = OpKind::Write;
        int rc = WSASend(fd, &writeWsaBuf, 1, nullptr, 0, &writeOp, nullptr);
        if (rc == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err != WSA_IO_PENDING) {
                auto callback = std::move(writeCallback);
                writeCallback = nullptr;
                if (callback)
                    callback(IoResult{writeOffset, false, err});
            }
        }
    }

    unsigned char* pendingReadBuf = nullptr;
    size_t pendingReadCap = 0;

    void onComplete(IocpOp* op, DWORD bytesTransferred, bool ok, int errorCode) override
    {
        if (op->kind == OpKind::Read) {
            if (!ok) {
                closed = true;
                auto callback = std::move(readCallback);
                readCallback = nullptr;
                if (callback)
                    callback(IoResult{0, false, errorCode});
                return;
            }
            auto callback = std::move(readCallback);
            readCallback = nullptr;
            if (callback)
                callback(IoResult{bytesTransferred, true, 0,
                                  reinterpret_cast<unsigned char*>(readWsaBuf.buf)});
        } else if (op->kind == OpKind::Write) {
            if (!ok) {
                closed = true;
                auto callback = std::move(writeCallback);
                writeCallback = nullptr;
                if (callback)
                    callback(IoResult{writeOffset, false, errorCode});
                return;
            }
            writeOffset += bytesTransferred;
            if (writeOffset >= writeLen) {
                auto callback = std::move(writeCallback);
                writeCallback = nullptr;
                if (callback)
                    callback(IoResult{writeLen, true, 0});
            } else {
                submitWrite();  // partial write; continue with the remainder
            }
        }
    }
};

TcpConnection::TcpConnection(IoLoop& loop, socket_t fd)
    : loop_(loop), fd_(fd), impl_(std::make_unique<Impl>(loop, fd))
{
    setNonBlocking(fd_);  // harmless with overlapped I/O; kept for parity
    setTcpNoDelay(fd_);
    impl_->ensureAssociated();
}

TcpConnection::~TcpConnection()
{
    close();
}

void TcpConnection::asyncRead(unsigned char* buffer, size_t bufferCapacity, IoCallback callback)
{
    if (impl_->closed) {
        callback(IoResult{0, false, WSAECONNABORTED});
        return;
    }
    if (!buffer) {
        if (impl_->ownReceive.empty())
            impl_->ownReceive.resize(16 * 1024);

        buffer = impl_->ownReceive.data();
        bufferCapacity = impl_->ownReceive.size();
    }
    impl_->pendingReadBuf = buffer;
    impl_->pendingReadCap = bufferCapacity;
    impl_->readCallback = std::move(callback);
    impl_->submitRead();
}

void TcpConnection::asyncWrite(const unsigned char* data, size_t length, IoCallback callback)
{
    if (impl_->closed) {
        callback(IoResult{0, false, WSAECONNABORTED});
        return;
    }
    if (length == 0) {
        callback(IoResult{0, true, 0});
        return;
    }
    impl_->writeBuf = data;
    impl_->writeLen = length;
    impl_->writeOffset = 0;
    impl_->writeCallback = std::move(callback);
    impl_->submitWrite();
}

size_t TcpConnection::tryWrite(const unsigned char*, size_t, const unsigned char*, size_t)
{
    // IOCP has no immediate send that leaves the buffers with the caller;
    // callers copy and use asyncWrite.
    return 0;
}

void TcpConnection::close()
{
    if (impl_->closed && fd_ == kInvalidSocket)
        return;

    impl_->closed = true;
    closeSocket(fd_);
    fd_ = kInvalidSocket;

    if (impl_->readCallback) {
        auto callback = std::move(impl_->readCallback);
        impl_->readCallback = nullptr;
        callback(IoResult{0, false, WSA_OPERATION_ABORTED});
    }
    if (impl_->writeCallback) {
        auto callback = std::move(impl_->writeCallback);
        impl_->writeCallback = nullptr;
        callback(IoResult{impl_->writeOffset, false, WSA_OPERATION_ABORTED});
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
    int len = sizeof(addr);
    if (fd_ == kInvalidSocket || getpeername(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        return peerAddress_;

    char host[NI_MAXHOST] = {0};
    char port[NI_MAXSERV] = {0};
    if (getnameinfo(reinterpret_cast<sockaddr*>(&addr), len, host, sizeof(host), port, sizeof(port),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return peerAddress_;
    }
    peerAddress_ = host;
    peerAddress_ += ':';
    peerAddress_ += port;
    return peerAddress_;
}

// ---------------------------------------------------------------------
// TcpListener
// ---------------------------------------------------------------------

struct TcpListener::Impl : IocpHandler
{
    IoLoop& loop;
    socket_t fd = kInvalidSocket;
    socket_t pendingAcceptSocket = kInvalidSocket;
    AcceptCallback onAccept;
    IocpOp acceptOp{};
    std::vector<char> acceptAddrBuf;  // holds local+remote sockaddr output from AcceptEx

    static constexpr size_t kAddrLen = sizeof(sockaddr_in) + 16;

    explicit Impl(IoLoop& l) : loop(l), acceptAddrBuf(kAddrLen * 2)
    {
        acceptOp.kind = OpKind::Accept;
    }

    void armAccept()
    {
        pendingAcceptSocket = ::socket(AF_INET, SOCK_STREAM, 0);
        if (pendingAcceptSocket == kInvalidSocket)
            return;

        DWORD bytesReceived = 0;
        std::memset(&acceptOp, 0, sizeof(WSAOVERLAPPED));
        acceptOp.kind = OpKind::Accept;
        BOOL rc = g_AcceptEx(fd, pendingAcceptSocket, acceptAddrBuf.data(), 0,
                             static_cast<DWORD>(kAddrLen), static_cast<DWORD>(kAddrLen),
                             &bytesReceived, &acceptOp);
        if (!rc) {
            int err = WSAGetLastError();
            if (err != ERROR_IO_PENDING) {
                closeSocket(pendingAcceptSocket);
                pendingAcceptSocket = kInvalidSocket;
                HTTP_LOG_WARN("AcceptEx failed: %d", err);
            }
        }
    }

    void onComplete(IocpOp*, DWORD, bool ok, int errorCode) override
    {
        if (ok) {
            setsockopt(pendingAcceptSocket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                       reinterpret_cast<const char*>(&fd), sizeof(fd));
            if (onAccept)
                onAccept(pendingAcceptSocket);
        } else {
            HTTP_LOG_WARN("AcceptEx completion failed: %d", errorCode);
            if (pendingAcceptSocket != kInvalidSocket)
                closeSocket(pendingAcceptSocket);
        }
        pendingAcceptSocket = kInvalidSocket;
        armAccept();  // keep accepting for the listener's lifetime
    }
};

TcpListener::TcpListener(IoLoop& loop, const std::string& host, uint16_t port, bool /*reusePort*/)
    : loop_(loop), impl_(std::make_unique<Impl>(loop))
{
    impl_->fd = ::socket(AF_INET, SOCK_STREAM, 0);
    setReuseAddr(impl_->fd);
    ensureExtensionFunctionsLoaded(impl_->fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (host.empty() || host == "0.0.0.0" || host == "*") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    }

    if (::bind(impl_->fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        HTTP_LOG_ERROR("bind(%s:%u) failed: %d", host.c_str(), port, WSAGetLastError());
    }
    if (::listen(impl_->fd, SOMAXCONN) != 0) {
        HTTP_LOG_ERROR("listen() failed: %d", WSAGetLastError());
    }

    CreateIoCompletionPort(reinterpret_cast<HANDLE>(impl_->fd), loop_.impl()->iocp,
                           reinterpret_cast<ULONG_PTR>(impl_.get()), 0);
}

TcpListener::~TcpListener()
{
    close();
}

void TcpListener::asyncAccept(AcceptCallback onAccept)
{
    impl_->onAccept = std::move(onAccept);
    impl_->armAccept();
}

void TcpListener::close()
{
    if (impl_->fd == kInvalidSocket)
        return;

    closeSocket(impl_->fd);
    impl_->fd = kInvalidSocket;
}

}  // namespace http
