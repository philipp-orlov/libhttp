// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// EXT-09: the bounded queues report what they refused and what failed.

#include "http/http_context.hpp"
#include "http/server.hpp"
#include "test_support.hpp"

#include <atomic>
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
    config.workerQueueCapacity = 2;
    config.reactorTaskCapacity = 2;
    http::Server server(config);

    std::atomic<int> reactorAccepted{0};
    std::atomic<int> reactorRefused{0};
    std::atomic<size_t> depthSeen{0};
    std::atomic<size_t> rejectedSeen{0};
    std::atomic<size_t> failedSeen{0};
    server.onGet("/burst", [&](http::HttpContext& context) {
        // Runs on the reactor thread: nothing posted to it can run before this returns.
        for (int index = 0; index < 5; ++index) {
            if (context.loop().tryPost([] {}))
                ++reactorAccepted;
            else
                ++reactorRefused;
        }
        context.loop().tryPost([] {});
        depthSeen = server.stats().reactorQueueDepth;
        rejectedSeen = server.stats().reactorTasksRejected;
        context.text("done");
    });
    server.onGet("/throw", [&](http::HttpContext& context) {
        context.loop().tryPost([] { throw std::runtime_error("task failed"); });
        context.text("posted");
    });
    auto serving = std::async(std::launch::async, [&] { server.run(); });

    expect(roundTrip(config.port, "GET /burst HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
                   .find("done") != std::string::npos,
           1, "burst answered");
    expect(reactorAccepted == 2 && reactorRefused == 3, 2, "queue capacity enforced");
    expect(depthSeen == 2, 3, "queue depth reported");
    expect(rejectedSeen == 4, 4, "rejected reactor tasks counted");

    roundTrip(config.port, "GET /throw HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    for (int wait = 0; wait < 100 && server.stats().reactorTasksFailed == 0; ++wait)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

    failedSeen = server.stats().reactorTasksFailed;
    expect(failedSeen == 1, 5, "failed reactor task counted");

    // Worker pool: one running (blocked), two queued, the next refused.
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::promise<void> started;
    expect(server.pool().tryEnqueue([&] {
        started.set_value();
        gate.wait();
    }),
           6, "first worker task");
    started.get_future().wait();
    expect(server.pool().tryEnqueue([] {}) && server.pool().tryEnqueue([] {}), 7, "queued tasks");
    expect(!server.pool().tryEnqueue([] {}), 8, "full worker queue refuses");
    http::Server::Stats stats = server.stats();
    expect(stats.workerTasksRejected == 1 && stats.workerQueueDepth == 2, 9,
           "worker queue statistics");
    release.set_value();

    server.stop();
    serving.get();
    std::puts("server stats: reactor and worker queue depth, rejected and failed counts passed");
}
