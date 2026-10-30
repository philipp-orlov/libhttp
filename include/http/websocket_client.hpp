// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A synchronous WebSocket client: connect, send, receive, close. It speaks the
// protocol http::Server's WebSocket routes serve (RFC 6455: masked client
// frames, ping/pong, fragmentation, close handshake).
//
// One thread may block in receive() while another calls sendBinary(), sendText()
// or ping(): sends are serialized by an internal mutex. Two threads must not
// call receive() at once. Buffers are kept between messages, so a warm
// connection that exchanges messages of a repeating size allocates nothing.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "http/memory_pool.hpp"

namespace http {

class WebSocketError : public std::runtime_error
{
public:
    enum class Kind
    {
        Connect,    // could not reach the server (resolve, connect, timeout)
        Handshake,  // the server did not upgrade; status() holds the HTTP status
        Tls,        // wss:// problems
        Io,         // the connection failed while in use
        Closed,     // the server closed it; closeCode() and closeReason()
        Protocol,   // the peer broke RFC 6455
        TooBig,     // a message above WebSocketClientOptions::maxMessageBytes
        Usage,      // a bad URL or a call on a closed client
    };
    WebSocketError(Kind kind, const std::string& what, int status = 0, uint16_t closeCode = 0,
                   std::string closeReason = {})
        : std::runtime_error(what), kind_(kind), status_(status), closeCode_(closeCode),
          closeReason_(std::move(closeReason))
    {}
    Kind kind() const noexcept { return kind_; }
    int status() const noexcept { return status_; }
    uint16_t closeCode() const noexcept { return closeCode_; }
    const std::string& closeReason() const noexcept { return closeReason_; }

private:
    Kind kind_;
    int status_;
    uint16_t closeCode_;
    std::string closeReason_;
};

struct WebSocketClientOptions
{
    std::vector<std::pair<std::string, std::string>> headers;  // Authorization, Cookie, ...
    std::vector<std::string> subprotocols;                      // offered, in order
    std::chrono::milliseconds connectTimeout{10000};
    size_t maxMessageBytes = 16 * 1024 * 1024;
    // wss:// needs a libhttp built with HTTP_WITH_SSL (connect() throws Kind::Tls otherwise).
    // The server's certificate chain and host name (or IP) are verified against caFile, or the
    // system's trusted roots when it is empty; insecureSkipVerify turns that off (tests only).
    std::string caFile;
    bool insecureSkipVerify = false;
    // Where the message buffer comes from; nullptr = BufferPool::shared().
    BufferPool* pool = nullptr;
};

// "ws://host[:port]/path?query", split.
struct WebSocketUrl
{
    bool secure = false;
    std::string host;  // without brackets for an IPv6 literal
    uint16_t port = 80;
    std::string target = "/";  // path and query, as sent in the request line
};
// Throws WebSocketError(Kind::Usage) for anything but ws:// or wss://, an empty host, a bad port.
WebSocketUrl parseWebSocketUrl(std::string_view url);

class WebSocketClient
{
public:
    WebSocketClient();
    ~WebSocketClient();
    WebSocketClient(WebSocketClient&&) noexcept;
    WebSocketClient& operator=(WebSocketClient&&) noexcept;
    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    // Throws WebSocketError (Connect, Handshake, Tls, Usage). A refused
    // handshake carries the HTTP status.
    void connect(std::string_view url, const WebSocketClientOptions& options = {});
    bool isOpen() const noexcept;
    // What the server chose from the offered sub-protocols ("" if none).
    const std::string& subprotocol() const noexcept;

    // One message (masked, as RFC 6455 requires of clients). Thread-safe.
    void sendBinary(const void* data, size_t size);
    void sendText(std::string_view text);
    void ping(std::string_view payload = {});

    struct Message
    {
        std::string_view data;
        bool binary = true;
    };
    // Waits for the next data message, at most until `deadline` (steady clock;
    // time_point::max() waits for ever). Answers pings, ignores pongs,
    // reassembles fragments, and sends a ping after every `pingEvery` of
    // silence (zero: never). The view is 8-aligned and valid until the next
    // receive(). Returns nullopt on timeout. Throws WebSocketError(Closed) when
    // the server closed, (Io) on socket errors, (Protocol) and (TooBig).
    std::optional<Message> receive(std::chrono::steady_clock::time_point deadline,
                                   std::chrono::milliseconds pingEvery = {});

    // True when the server's Close frame, or the end of the stream, is already
    // waiting (non-blocking; data that is not a close does not matter).
    bool peerClosed();
    // Sends a Close frame (best effort), then closes the socket.
    void close(uint16_t code = 1000, std::string_view reason = {}) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace http
