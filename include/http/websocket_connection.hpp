// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace http {

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
    // the task is dropped rather than run anywhere else.
    bool post(std::function<void()> task)
    {
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
    void close()
    {
        if (closeFunction_)
            closeFunction_();
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
    void bindCloseFunction(std::function<void()> closeFunction)
    {
        closeFunction_ = std::move(closeFunction);
    }
    void bindPostFunction(std::function<bool(std::function<void()>)> postFunction)
    {
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
        closeFunction_ = nullptr;
        postFunction_ = nullptr;
    }

private:
    MessageHandler onMessage_;
    CloseHandler onClose_;
    std::function<void(std::string_view, bool)> sendFunction_;
    std::function<void()> closeFunction_;
    std::function<bool(std::function<void()>)> postFunction_;
    std::vector<std::pair<std::string, std::string>> routeParams_;
    std::string remoteAddress_;
};

using WebSocketHandler = std::function<void(std::shared_ptr<WebSocketConnection>)>;

}  // namespace http
