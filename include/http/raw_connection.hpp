// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A connection handed over whole by Server::onRaw: from the first byte of the request that a raw route claimed, the
// handler reads and writes the byte stream itself, and the server does no HTTP on it any more. For reverse proxies and
// tunnels, which pass a client's requests on as they arrived (keep-alive, upgrades and all) to somewhere else.
//
// Like WebSocketConnection: the handlers run on the connection's loop, and everything but post() must be called there.
// Work on another thread comes back through post().

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace http {

class RawConnection : public std::enable_shared_from_this<RawConnection> {
public:
    using DataHandler = std::function<void(std::string_view data)>;
    using Handler = std::function<void()>;

    // Bytes from the client, starting with the request the route claimed. `data` is valid during the call only.
    void onData(DataHandler handler) { onData_ = std::move(handler); }
    // The client will send no more (it shut down its side); it may still read. Unset: that closes the connection.
    void onEof(Handler handler) { onEof_ = std::move(handler); }
    // What was written has all gone to the socket: for a writer that waits to be told it may write more. Never called from
    // inside write() or onData.
    void onDrained(Handler handler) { onDrained_ = std::move(handler); }
    // Once, when the connection is gone, however it went.
    void onClose(Handler handler) { onClose_ = std::move(handler); }

    // Queues bytes for the client. False when the connection is gone. Nothing limits the queue: a writer that can
    // produce faster than the client reads watches queuedBytes() and onDrained.
    bool write(std::string_view data) { return writeFunction_ ? writeFunction_(data) : false; }
    // Bytes queued and not yet taken by the socket.
    size_t queuedBytes() const { return queuedFunction_ ? queuedFunction_() : 0; }
    // Stops and restarts reading from the client: what is unread waits in the kernel, which slows the client down.
    void pauseReading() {
        if (pauseFunction_)
            pauseFunction_(true);
    }
    void resumeReading() {
        if (pauseFunction_)
            pauseFunction_(false);
    }
    // Closes once what is queued has been written.
    void close() {
        if (closeFunction_)
            closeFunction_(false);
    }
    // Closes at once.
    void abort() {
        if (closeFunction_)
            closeFunction_(true);
    }
    // Any thread, also after the connection closed: runs `task` on the connection's loop. False when it cannot.
    bool post(std::function<void()> task) {
        std::lock_guard<std::mutex> lock(postMutex_);
        return postFunction_ ? postFunction_(std::move(task)) : false;
    }
    // "host:port" of the client.
    const std::string& remoteAddress() const { return remoteAddress_; }

    // --- internal wiring, used by the connection session ---
    void bind(std::function<bool(std::string_view)> write, std::function<size_t()> queued,
              std::function<void(bool)> pause, std::function<void(bool)> close,
              std::function<bool(std::function<void()>)> post, std::string remoteAddress) {
        writeFunction_ = std::move(write);
        queuedFunction_ = std::move(queued);
        pauseFunction_ = std::move(pause);
        closeFunction_ = std::move(close);
        remoteAddress_ = std::move(remoteAddress);
        std::lock_guard<std::mutex> lock(postMutex_);
        postFunction_ = std::move(post);
    }
    DataHandler& dataCallback() { return onData_; }
    Handler& eofCallback() { return onEof_; }
    Handler& drainedCallback() { return onDrained_; }
    Handler& closeCallback() { return onClose_; }
    // Called once the connection is gone, after onClose ran: drops every callback so a handler that captures its own
    // connection cannot keep it alive.
    void releaseCallbacks() {
        onData_ = nullptr;
        onEof_ = nullptr;
        onDrained_ = nullptr;
        onClose_ = nullptr;
        writeFunction_ = nullptr;
        queuedFunction_ = nullptr;
        pauseFunction_ = nullptr;
        closeFunction_ = nullptr;
        std::lock_guard<std::mutex> lock(postMutex_);
        postFunction_ = nullptr;
    }

private:
    DataHandler onData_;
    Handler onEof_;
    Handler onDrained_;
    Handler onClose_;
    std::function<bool(std::string_view)> writeFunction_;
    std::function<size_t()> queuedFunction_;
    std::function<void(bool)> pauseFunction_;
    std::function<void(bool)> closeFunction_;
    std::mutex postMutex_;
    std::function<bool(std::function<void()>)> postFunction_;
    std::string remoteAddress_;
};

// What a raw route decides on: the request head as received, through the blank line that ends it.
using RawMatch = std::function<bool(std::string_view head)>;
// Called once, on the connection's loop, before the first bytes are delivered: set the handlers here.
using RawHandler = std::function<void(std::string_view head, std::shared_ptr<RawConnection> connection)>;

struct RawRouteOptions {
    // A connection that carries nothing either way for this long is closed. 0: ServerConfig::keepAliveTimeoutSeconds.
    int idleTimeoutSeconds = 0;
};

// A header's value from a raw request head (the first of that name, case-insensitive, trimmed); "" if absent.
std::string_view rawHeader(std::string_view head, std::string_view name);

} // namespace http
