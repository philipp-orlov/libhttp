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

    server.stop();
    serving.get();
    std::puts("keep-alive: per-request deadline passed");
}
