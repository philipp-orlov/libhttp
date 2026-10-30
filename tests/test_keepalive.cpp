// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// EXT-04: the request deadline belongs to one request, not to the connection.

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
}  // namespace

int main()
{
    http::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = freePort();
    config.ioThreads = 1;
    config.workerThreads = 1;
    config.keepAliveTimeoutSeconds = 10;
    config.requestTimeoutSeconds = 1;
    http::Server server(config);
    server.onGet("/ping", [](http::HttpContext& context) { context.text("pong"); });
    auto serving = std::async(std::launch::async, [&] { server.run(); });

    const std::string request = "GET /ping HTTP/1.1\r\nHost: x\r\n\r\n";
    const int client = connectTo(config.port, 5);
    for (int round = 0; round < 3; ++round) {
        sendAll(client, request);
        expect(readUntil(client, "pong").find("pong") != std::string::npos, 1,
               "request on a connection idle for longer than requestTimeoutSeconds");
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }
    ::close(client);

    // A request that never completes is still cut off after requestTimeoutSeconds.
    const int slow = connectTo(config.port, 5);
    sendAll(slow, "GET /ping HTTP/1.1\r\nHost: x\r\n");
    const auto started = std::chrono::steady_clock::now();
    char byte;
    const ssize_t closed = ::recv(slow, &byte, 1, 0);
    const auto waited = std::chrono::steady_clock::now() - started;
    expect(closed == 0, 2, "stalled request closed by the server");
    expect(waited < std::chrono::seconds(3), 3, "stalled request closed near the request deadline");
    ::close(slow);

    // A close in a later Connection line ends the connection: the pipelined
    // request behind it is never dispatched.
    const int closing = connectTo(config.port, 5);
    sendAll(closing, "GET /ping HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n"
                     "Connection: close\r\n\r\nGET /ping HTTP/1.1\r\nHost: x\r\n\r\n");
    std::string all;
    char chunk[512];
    for (ssize_t got; (got = ::recv(closing, chunk, sizeof chunk, 0)) > 0;)
        all.append(chunk, static_cast<size_t>(got));
    size_t responses = 0;
    for (size_t at = all.find("HTTP/1.1 200"); at != std::string::npos;
         at = all.find("HTTP/1.1 200", at + 1))
        ++responses;
    expect(responses == 1, 4, "exactly one response before a Connection: close in a later line");
    ::close(closing);

    server.stop();
    serving.get();
    std::puts("keep-alive: per-request deadline passed");
}
