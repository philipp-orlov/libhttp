// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// EXT-03: a Session (and the request/context an async handler holds) must not
// be recycled for another connection while that handler has not called done.

#include "http/http_context.hpp"
#include "http/server.hpp"
#include "test_support.hpp"

#include <atomic>
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
    config.workerThreads = 2;
    // The idle timer closes the connection while the handler is still busy.
    config.keepAliveTimeoutSeconds = 1;
    config.requestTimeoutSeconds = 1;
    http::Server server(config);
    std::atomic<int> corrupted{0};
    std::atomic<int> finished{0};
    server.onPost("/slow", [&](http::HttpContext& context, std::function<void()> done) {
        const std::string expected(context.request().body.view());
        server.pool().enqueue([&, expected, done] {
            std::this_thread::sleep_for(std::chrono::milliseconds(2500));
            context.loop().post([&, expected, done] {
                if (context.request().body.view() != expected ||
                    context.request().path != "/slow")
                    ++corrupted;

                context.text("slow");
                done();
                ++finished;
            });
        });
    });
    server.onPost("/fast", [](http::HttpContext& context) { context.text("fast"); });
    auto serving = std::async(std::launch::async, [&] { server.run(); });

    const std::string slowBody(3000, 'A');
    const int slow = connectTo(config.port, 10);
    sendAll(slow, "POST /slow HTTP/1.1\r\nHost: x\r\nContent-Length: 3000\r\n\r\n" + slowBody);
    // Past the 1 s timer: the connection has been closed by the server.
    std::this_thread::sleep_for(std::chrono::milliseconds(1600));

    std::vector<int> others;
    for (int index = 0; index < 8; ++index) {
        others.push_back(connectTo(config.port, 10));
        sendAll(others.back(), "POST /fast HTTP/1.1\r\nHost: x\r\nContent-Length: 3000\r\n\r\n" +
                                   std::string(3000, 'B'));
        if (readUntil(others.back(), "fast").find("fast") == std::string::npos)
            std::_Exit(1);
    }
    for (int deadline = 0; finished < 1 && deadline < 100; ++deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    if (finished != 1 || corrupted != 0) {
        std::fprintf(stderr, "finished=%d corrupted=%d\n", finished.load(), corrupted.load());
        std::_Exit(2);
    }
    // The Session went back to the pool once the handler was done.
    for (int index = 0; index < 4; ++index) {
        if (roundTrip(config.port, "POST /fast HTTP/1.1\r\nHost: x\r\nConnection: close\r\n"
                                   "Content-Length: 0\r\n\r\n")
                .find("fast") == std::string::npos)
            std::_Exit(3);
    }
    ::close(slow);
    for (int socket : others)
        ::close(socket);

    server.stop();
    serving.get();
    std::puts("zombie session: request state stable until the handler is done passed");
}
