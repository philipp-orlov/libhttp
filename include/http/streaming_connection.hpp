// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace http {

class HttpContext;

// User-facing handle for a long-lived, server-push HTTP response opened by
// Server::onStream() -- e.g. a multipart/x-mixed-replace MJPEG feed. Handed
// to the route handler once, synchronously, the same way WebSocketConnection
// is handed to a WebSocket route: the handler configures the response
// (status/headers) on the HttpContext it is also given, keeps this
// connection around, and pushes bytes to it later from wherever the data
// comes from.
//
// Unlike WebSocketConnection, writes here are raw, unframed bytes -- the
// handler owns the wire format entirely (a multipart boundary, in the MJPEG
// case) -- and, like a live video tick, a write is dropped rather than
// queued when the previous one has not finished: a frame that waits for a
// slow client is stale by the time it would go out.
class StreamingConnection : public std::enable_shared_from_this<StreamingConnection>
{
public:
    using CloseHandler = std::function<void()>;

    void onClose(CloseHandler handler) { onClose_ = std::move(handler); }

    // write()/post() touch the connection directly, so they are only safe on
    // the loop that owns it. Work handed to a thread pool must come back
    // through post() before calling write() -- exactly the WebSocketConnection
    // contract.
    bool post(std::function<void()> task) { return postFunction_ ? postFunction_(std::move(task)) : false; }

    // Raw bytes onto the wire. Returns false, and drops the data, when a
    // previous write from this connection has not completed yet or the
    // connection is gone; the caller decides what "dropped" means for it
    // (for a video tick, simply the next one).
    bool write(const void* data, size_t length) { return writeFunction_ ? writeFunction_(data, length) : false; }

    void close() {
        if (closeFunction_)
            closeFunction_();
    }

    // --- internal wiring, used by the connection session ---
    void bindWriteFunction(std::function<bool(const void*, size_t)> writeFunction) {
        writeFunction_ = std::move(writeFunction);
    }
    void bindCloseFunction(std::function<void()> closeFunction) { closeFunction_ = std::move(closeFunction); }
    void bindPostFunction(std::function<bool(std::function<void()>)> postFunction) {
        postFunction_ = std::move(postFunction);
    }
    CloseHandler& closeCallback() { return onClose_; }
    // Called once the connection is gone, after onClose ran: drops every
    // callback so a handler that captures its own connection cannot keep it alive.
    void releaseCallbacks()
    {
        onClose_ = nullptr;
        writeFunction_ = nullptr;
        closeFunction_ = nullptr;
        postFunction_ = nullptr;
    }

private:
    CloseHandler onClose_;
    std::function<bool(const void*, size_t)> writeFunction_;
    std::function<void()> closeFunction_;
    std::function<bool(std::function<void()>)> postFunction_;
};

// Called once, synchronously, on the connection's own loop: the handler sets
// up the response (e.g. `context.header("Content-Type", "multipart/x-mixed-"
// "replace; boundary=...")`) and keeps `connection` for later writes. The
// head is sent immediately after this call returns, so nothing should be
// written to `connection` before then.
using StreamHandler = std::function<void(HttpContext& context, std::shared_ptr<StreamingConnection> connection)>;

}  // namespace http
