// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

//
// Minimal plaintext benchmark server: the libhttp equivalent of
// uWebSockets' HelloWorld, for load-testing the request path with
// bombardier/wrk.
//
//   http_bench_hello [threads] [port]
//
// GET /            -> 200 "Hello, World!" (text/plain)
// GET /json        -> 200 {"message":"Hello, World!"}
// WebSocket /      -> echo (e.g. uWebSockets' benchmarks/load_test)

#include <csignal>
#include <cstdio>
#include <cstdlib>

#include "http/logger.hpp"
#include "http/server.hpp"
#include "json/json.hpp"

namespace {
volatile std::sig_atomic_t stopRequested = 0;
void handleSignal(int)
{
    stopRequested = 1;
}
}  // namespace

int main(int argc, char** argv)
try {
    http::Pal pal;
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    http::Logger::instance().setLevel(http::LogLevel::Warn);

    http::ServerConfig config;
    config.ioThreads = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 0;
    config.port = argc > 2 ? static_cast<uint16_t>(std::atoi(argv[2])) : 8080;
    config.maxConnectionsPerLoop = 4000;
    config.reactorTimerCapacity = 4096;

    http::Server server(config);
    server.onGet("/", [](http::HttpContext& context) { context.text("Hello, World!", "text/plain"); })
        .onGet("/json", [](http::HttpContext& context) {
            context.json(json::Json{{"message", "Hello, World!"}});
        })
        .onWebSocket("/", [](std::shared_ptr<http::WebSocketConnection> socket) {
            socket->onMessage([socket](std::string_view data, bool binary) { socket->send(data, binary); });
        });
    std::fprintf(stderr, "http_bench_hello: %u thread(s) on port %u\n", config.ioThreads, config.port);
    server.run([] { return stopRequested != 0; });
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
