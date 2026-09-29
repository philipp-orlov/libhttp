// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A handler that captures its own connection (the natural way to write an
// echo handler) must not keep the connection alive after the client is gone.

#include "http/http_context.hpp"
#include "http/server.hpp"
#include "test_support.hpp"

#include <cstdlib>
#include <future>

using namespace testsupport;

namespace {
void expect(bool condition, int code, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::_Exit(code);
    }
}

template <class Weak>
bool expiresSoon(const Weak& weak)
{
    for (int wait = 0; wait < 100; ++wait) {
        if (weak.expired())
            return true;

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

// The handler runs just after the head reaches the client.
template <class Weak>
bool aliveSoon(const Weak& weak)
{
    for (int wait = 0; wait < 100; ++wait) {
        if (!weak.expired())
            return true;

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}
}  // namespace

int main()
{
    http::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = freePort();
    config.ioThreads = 1;
    config.workerThreads = 1;
    http::Server server(config);
    std::weak_ptr<http::WebSocketConnection> websocket;
    std::weak_ptr<http::StreamingConnection> stream;
    server.onWebSocket("/ws", [&](std::shared_ptr<http::WebSocketConnection> connection) {
        websocket = connection;
        connection->onMessage([connection](std::string_view data, bool binary) {
            connection->send(data, binary);
        });
        connection->onClose([connection] {});
    });
    server.onStream("/stream", [&](http::HttpContext& context,
                                   std::shared_ptr<http::StreamingConnection> connection) {
        stream = connection;
        context.response().setHeader("Content-Type", "text/plain");
        connection->onClose([connection] {});
    });
    auto serving = std::async(std::launch::async, [&] { server.run(); });

    const int client = connectTo(config.port);
    sendAll(client,
            "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
            "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    expect(readUntil(client, "\r\n\r\n").find("101") != std::string::npos, 1, "handshake");
    expect(aliveSoon(websocket), 2, "connection alive while open");
    ::close(client);
    expect(expiresSoon(websocket), 3, "WebSocket connection released after close");

    const int viewer = connectTo(config.port);
    sendAll(viewer, "GET /stream HTTP/1.1\r\nHost: x\r\n\r\n");
    expect(readUntil(viewer, "\r\n\r\n").find("200") != std::string::npos, 4, "stream head");
    expect(aliveSoon(stream), 5, "stream alive while open");
    ::close(viewer);
    expect(expiresSoon(stream), 6, "stream connection released after close");

    server.stop();
    serving.get();
    std::puts("callback release: self-capturing WebSocket and stream handlers are freed on close");
}
