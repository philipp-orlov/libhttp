// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "http/buffer.hpp"
#include "http/http_types.hpp"

namespace http {

// What a WebSocket route's accept hook sees of the handshake. Valid during
// the hook only.
struct WebSocketHandshakeRequest
{
    const HttpRequest& request;  // method, path, headers (request.headers.get(name))
    std::string_view remoteAddress;
    const std::vector<std::pair<std::string, std::string>>& routeParams;
};

// What the accept hook decides.
struct WebSocketAccept
{
    int status = 101;          // anything else refuses the handshake with that status
    std::string subprotocol;   // echoed in Sec-WebSocket-Protocol when it was offered; empty = none
    std::shared_ptr<void> userData;  // handed to the handler as WebSocketConnection::userData()
};

// Per-route settings for Server::onWebSocket.
struct WebSocketRouteOptions
{
    // Unset: every handshake is accepted.
    std::function<WebSocketAccept(const WebSocketHandshakeRequest&)> accept;
    // If non-empty the client must offer one of these in Sec-WebSocket-Protocol
    // (else 400); the first of the client's offers that is listed is chosen.
    std::vector<std::string> subprotocols;
    // 0 = ServerConfig::maxWebSocketMessageBytes (the larger of a route's
    // value and the server's is never exceeded: a route can only tighten or,
    // up to 16 MiB, raise the limit for its own connections).
    size_t maxMessageBytes = 0;
    // 0 = ServerConfig::keepAliveTimeoutSeconds. Any frame from the client
    // (a ping included) restarts it.
    int idleTimeoutSeconds = 0;
};

// User-facing handle for one upgraded WebSocket connection. The
// send/close implementations are injected by the owning connection
// session (see server.cpp) so this header stays free of any dependency on
// the internal session/reactor machinery.
class WebSocketConnection : public std::enable_shared_from_this<WebSocketConnection>
{
public:
    using MessageHandler = std::function<void(std::string_view data, bool isBinary)>;
    using CloseHandler = std::function<void()>;

    void onMessage(MessageHandler handler) { onMessage_ = std::move(handler); }
    void onClose(CloseHandler handler) { onClose_ = std::move(handler); }

    // send() and close() touch the connection's write buffer directly, so
    // they are only safe on the loop that owns this connection -- which is
    // the thread the message and close handlers already run on. Work handed
    // to a thread pool must come back through post() before answering.
    // Returns false if the loop is gone or its queue is full, in which case
    // the task is dropped rather than run anywhere else. Callable from any
    // thread, also while the connection closes or after it has.
    bool post(std::function<void()> task)
    {
        std::lock_guard<std::mutex> lock(postMutex_);
        return postFunction_ ? postFunction_(std::move(task)) : false;
    }

    void send(std::string_view data, bool binary = false)
    {
        if (sendFunction_)
            sendFunction_(data, binary);
    }

    // Raw-buffer overload for binary payloads (e.g. an image) that don't
    // originate as a string_view -- a pointer + length pair, matching the
    // shape a decoder or capture pipeline would hand you.
    void send(const void* data, size_t length, bool binary = true)
    {
        send(std::string_view(reinterpret_cast<const char*>(data), length), binary);
    }
    // Loop thread only, like send(string_view): queues the pooled block itself,
    // with no copy at any size, and returns it to its pool once written. The
    // frame's order relative to other sends is kept.
    void send(Buffer&& frame, bool binary = true)
    {
        if (sendBufferFunction_)
            sendBufferFunction_(std::move(frame), binary);
    }
    // Starts the closing handshake with a status code (RFC 6455 section 7.4)
    // and a reason of at most 123 bytes. Loop thread only, like send().
    void close(uint16_t code = 1000, std::string_view reason = {})
    {
        if (closeFunction_)
            closeFunction_(code, reason);
    }

    // The sub-protocol the handshake agreed on ("" if none) and whatever the
    // route's accept hook returned in WebSocketAccept::userData.
    const std::string& subprotocol() const { return subprotocol_; }
    const std::shared_ptr<void>& userData() const { return userData_; }
    void setHandshake(std::string subprotocol, std::shared_ptr<void> userData)
    {
        subprotocol_ = std::move(subprotocol);
        userData_ = std::move(userData);
    }

    std::string_view routeParam(std::string_view name) const
    {
        for (auto& kv : routeParams_) {
            if (kv.first == name)
                return kv.second;
        }
        return {};
    }

    // "host:port" of the peer that upgraded to this connection, set once
    // at handshake time (see Session::upgradeToWebSocket in server.cpp).
    const std::string& remoteAddress() const { return remoteAddress_; }
    void setRemoteAddress(std::string address) { remoteAddress_ = std::move(address); }

    // --- internal wiring, used by the connection session ---
    void bindSendFunction(std::function<void(std::string_view, bool)> sendFunction)
    {
        sendFunction_ = std::move(sendFunction);
    }
    void bindSendBufferFunction(std::function<void(Buffer&&, bool)> sendBufferFunction)
    {
        sendBufferFunction_ = std::move(sendBufferFunction);
    }
    void bindCloseFunction(std::function<void(uint16_t, std::string_view)> closeFunction)
    {
        closeFunction_ = std::move(closeFunction);
    }
    void bindPostFunction(std::function<bool(std::function<void()>)> postFunction)
    {
        std::lock_guard<std::mutex> lock(postMutex_);
        postFunction_ = std::move(postFunction);
    }
    void setRouteParams(std::vector<std::pair<std::string, std::string>> params)
    {
        routeParams_ = std::move(params);
    }
    MessageHandler& messageCallback() { return onMessage_; }
    CloseHandler& closeCallback() { return onClose_; }
    // Called once the connection is gone, after onClose ran: drops every
    // callback so that a handler capturing its own connection (the usual
    // `[connection]` in onMessage) does not keep it alive forever.
    void releaseCallbacks()
    {
        onMessage_ = nullptr;
        onClose_ = nullptr;
        sendFunction_ = nullptr;
        sendBufferFunction_ = nullptr;
        closeFunction_ = nullptr;
        userData_ = nullptr;
        // post() runs on other threads: the one callback dropped under the lock.
        std::lock_guard<std::mutex> lock(postMutex_);
        postFunction_ = nullptr;
    }

private:
    MessageHandler onMessage_;
    CloseHandler onClose_;
    std::function<void(std::string_view, bool)> sendFunction_;
    std::function<void(Buffer&&, bool)> sendBufferFunction_;
    std::function<void(uint16_t, std::string_view)> closeFunction_;
    std::string subprotocol_;
    std::shared_ptr<void> userData_;
    std::mutex postMutex_;
    std::function<bool(std::function<void()>)> postFunction_;
    std::vector<std::pair<std::string, std::string>> routeParams_;
    std::string remoteAddress_;
};

using WebSocketHandler = std::function<void(std::shared_ptr<WebSocketConnection>)>;

}  // namespace http
