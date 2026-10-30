// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// EXT-01: a declared large body must not take the shared buffer pool hostage,
// and exhaustion must be answered rather than silently dropped.

#include "http/http_context.hpp"
#include "http/server.hpp"
#include "test_support.hpp"

#include <cstdlib>
#include <future>
#include <vector>

using namespace testsupport;

int main()
{
    http::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = freePort();
    config.ioThreads = 1;
    config.workerThreads = 1;
    config.maxBodyBytes = 32ull << 20;
    config.buffers.maxBytes = 64ull << 20;
    http::Server server(config);
    server.onGet("/ping", [](http::HttpContext& context) { context.text("pong"); });
    server.onPost("/echo", [](http::HttpContext& context) {
        context.text("echo:" + std::to_string(context.request().body.size()));
    });
    auto serving = std::async(std::launch::async, [&] { server.run(); });

    const std::string declare =
        "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 30000000\r\n\r\n";
    std::vector<int> stalled;
    for (int index = 0; index < 4; ++index) {
        stalled.push_back(connectTo(config.port));
        sendAll(stalled.back(), declare);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Four declarations of 30 MB, none with any body: a real upload still fits.
    {
        const std::string body(1 << 20, 'x');
        const std::string reply =
            roundTrip(config.port, "POST /echo HTTP/1.1\r\nHost: x\r\nConnection: close\r\n"
                                  "Content-Length: 1048576\r\n\r\n" + body);
        if (reply.find("200 OK") == std::string::npos || reply.find("echo:1048576") == std::string::npos) {
            std::fprintf(stderr, "upload failed: %s\n", reply.substr(0, 200).c_str());
            std::_Exit(1);
        }
    }
    if (server.bufferStats().backingBytes > 16ull << 20)
        std::_Exit(2);

    for (int socket : stalled)
        ::close(socket);

    // Four clients that have sent some of a 30 MB body: only one block of that
    // class fits, the others are told to come back later.
    int refused = 0;
    int served = 0;
    std::vector<int> senders;
    for (int index = 0; index < 4; ++index) {
        const int socket = connectTo(config.port, 1);
        sendAll(socket, declare);
        sendAll(socket, std::string(20000, 'y'));
        senders.push_back(socket);
    }
    for (int socket : senders) {
        const std::string reply = readUntil(socket, "\r\n\r\n");
        if (reply.find("503 Service Unavailable") != std::string::npos &&
            reply.find("Retry-After:") != std::string::npos)
            ++refused;
        else if (reply.empty())
            ++served;  // still waiting for the rest of its body
        ::close(socket);
    }
    if (refused < 1 || served < 1) {
        std::fprintf(stderr, "refused=%d waiting=%d\n", refused, served);
        std::_Exit(3);
    }
    // The pool recovers once they are gone.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (roundTrip(config.port, "GET /ping HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            .find("pong") == std::string::npos)
        std::_Exit(4);

    server.stop();
    serving.get();
    std::puts("pool admission: header-only declarations, 503 on exhaustion, recovery passed");
}
