// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// EXT-02: Host and Origin allowlists, CORS preflight header allowlist.

#include "http/http_context.hpp"
#include "http/server.hpp"
#include "test_support.hpp"

#include <cstdlib>
#include <future>

using namespace testsupport;

namespace {
std::string get(uint16_t port, const std::string& host, const std::string& extra = {})
{
    return roundTrip(port, "GET /ping HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n" +
                               extra + "\r\n");
}

std::string handshake(uint16_t port, const std::string& origin)
{
    const int socket = connectTo(port);
    sendAll(socket,
            "GET /ws HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
            "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" +
                (origin.empty() ? std::string() : "Origin: " + origin + "\r\n") + "\r\n");
    std::string reply = readUntil(socket, "\r\n\r\n");
    ::close(socket);
    return reply;
}

void expect(bool condition, int code, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::_Exit(code);
    }
}
}  // namespace

int main()
{
    http::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = freePort();
    config.ioThreads = 1;
    config.workerThreads = 1;
    config.corsAllowOrigin = "*";
    config.allowedHosts = {"localhost", "127.0.0.1", "[::1]"};
    config.allowedOrigins = {"http://good.example:8080"};
    http::Server server(config);
    server.onGet("/ping", [](http::HttpContext& context) { context.text("pong"); });
    server.onWebSocket("/ws", [](std::shared_ptr<http::WebSocketConnection>) {});
    auto serving = std::async(std::launch::async, [&] { server.run(); });

    const std::string port = std::to_string(config.port);
    expect(get(config.port, "127.0.0.1:" + port).find("200 OK") != std::string::npos, 1, "listed host");
    expect(get(config.port, "LOCALHOST").find("200 OK") != std::string::npos, 2, "case-insensitive host");
    expect(get(config.port, "[::1]:" + port).find("200 OK") != std::string::npos, 3, "IPv6 host");
    expect(get(config.port, "evil.example").find("421 Misdirected Request") != std::string::npos, 4,
           "rebinding host refused");
    expect(get(config.port, "localhost.evil.example:" + port).find("421") != std::string::npos, 5,
           "suffix host refused");

    expect(handshake(config.port, "").find("101 Switching") != std::string::npos, 6,
           "no Origin (non-browser) allowed");
    expect(handshake(config.port, "http://good.example:8080").find("101 Switching") !=
               std::string::npos,
           7, "listed origin allowed");
    expect(handshake(config.port, "http://evil.example").find("403 Forbidden") != std::string::npos, 8,
           "foreign origin refused");
    expect(handshake(config.port, "null").find("403 Forbidden") != std::string::npos, 9,
           "null origin refused");

    // Preflight: only allowlisted request headers are granted.
    const std::string preflight = roundTrip(
        config.port,
        "OPTIONS /ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n"
        "Access-Control-Request-Headers: content-type, x-evil, Authorization\r\n\r\n");
    const size_t at = preflight.find("Access-Control-Allow-Headers: ");
    expect(at != std::string::npos, 10, "preflight answers");
    const std::string line = preflight.substr(at, preflight.find("\r\n", at) - at);
    expect(line == "Access-Control-Allow-Headers: content-type, Authorization", 11,
           "preflight header allowlist");

    server.stop();
    serving.get();
    std::puts("origin/host: allowlists and CORS preflight header filtering passed");
}
