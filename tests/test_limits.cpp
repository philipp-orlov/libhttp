// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// EXT-05: connection limits, header read deadline, out-of-descriptor handling.

#include "http/http_context.hpp"
#include "http/server.hpp"
#include "test_support.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <future>
#include <sstream>
#include <vector>

using namespace testsupport;

namespace {
void expect(bool condition, int code, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::_Exit(code);
    }
}

http::ServerConfig baseConfig()
{
    http::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = freePort();
    config.ioThreads = 1;
    config.workerThreads = 1;
    return config;
}

const std::string kPing = "GET /ping HTTP/1.1\r\nHost: x\r\n\r\n";

// Holds `count` idle keep-alive connections that each completed one request.
std::vector<int> hold(uint16_t port, int count)
{
    std::vector<int> sockets;
    for (int index = 0; index < count; ++index) {
        sockets.push_back(connectTo(port, 3));
        sendAll(sockets.back(), kPing);
        expect(readUntil(sockets.back(), "pong").find("pong") != std::string::npos, 1,
               "connection within the limit is served");
    }
    return sockets;
}

void closeAll(std::vector<int>& sockets)
{
    for (int socket : sockets)
        ::close(socket);

    sockets.clear();
}

void limitTests()
{
    // Per-loop cap: the connection over it is told 503, not just reset.
    {
        http::ServerConfig config = baseConfig();
        config.maxConnectionsPerLoop = 3;
        config.reactorTimerCapacity = 16;
        http::Server server(config);
        server.onGet("/ping", [](http::HttpContext& context) { context.text("pong"); });
        auto serving = std::async(std::launch::async, [&] { server.run(); });
        std::vector<int> held = hold(config.port, 3);
        const int extra = connectTo(config.port, 3);
        const std::string reply = readUntil(extra, "\r\n\r\n");
        expect(reply.find("503 Service Unavailable") != std::string::npos, 2,
               "over-capacity connection answered 503");
        ::close(extra);
        closeAll(held);
        server.stop();
        serving.get();
    }
    // Per-address cap.
    {
        http::ServerConfig config = baseConfig();
        config.maxConnectionsPerIp = 2;
        http::Server server(config);
        server.onGet("/ping", [](http::HttpContext& context) { context.text("pong"); });
        auto serving = std::async(std::launch::async, [&] { server.run(); });
        std::vector<int> held = hold(config.port, 2);
        const int extra = connectTo(config.port, 3);
        sendAll(extra, kPing);
        expect(readUntil(extra, "\r\n\r\n").find("503") != std::string::npos, 3,
               "third connection from one address refused");
        ::close(extra);
        // Freeing a slot lets the address connect again.
        ::close(held.back());
        held.pop_back();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const int again = connectTo(config.port, 3);
        sendAll(again, kPing);
        expect(readUntil(again, "pong").find("pong") != std::string::npos, 4,
               "address may reconnect after closing one");
        ::close(again);
        closeAll(held);
        server.stop();
        serving.get();
    }
}

void headerDeadlineTest()
{
    http::ServerConfig config = baseConfig();
    config.headerReadTimeoutSeconds = 1;
    config.requestTimeoutSeconds = 30;
    config.keepAliveTimeoutSeconds = 30;
    http::Server server(config);
    server.onGet("/ping", [](http::HttpContext& context) { context.text("pong"); });
    auto serving = std::async(std::launch::async, [&] { server.run(); });

    // Headers dribbled one byte at a time are cut off after ~1 s in total,
    // however often a byte arrives.
    const int slow = connectTo(config.port, 5);
    const auto started = std::chrono::steady_clock::now();
    const std::string head = "GET /ping HTTP/1.1\r\nHost: x\r\nX-Slow: ";
    bool closed = false;
    for (size_t index = 0; index < head.size() + 40 && !closed; ++index) {
        const char byte = index < head.size() ? head[index] : 'a';
        if (::send(slow, &byte, 1, MSG_NOSIGNAL) < 0)
            closed = true;

        pollfd descriptor{slow, POLLIN, 0};
        if (::poll(&descriptor, 1, 150) > 0) {
            char probe;
            closed = ::recv(slow, &probe, 1, 0) <= 0;
        }
    }
    const auto waited = std::chrono::steady_clock::now() - started;
    expect(closed, 5, "slow headers cut off");
    expect(waited < std::chrono::seconds(4), 6, "slow headers cut off near the header deadline");
    ::close(slow);

    // A complete request is not affected, and a body may take longer than the header deadline.
    const int fine = connectTo(config.port, 5);
    sendAll(fine, kPing);
    expect(readUntil(fine, "pong").find("pong") != std::string::npos, 7, "normal request");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    sendAll(fine, kPing);
    expect(readUntil(fine, "pong").find("pong") != std::string::npos, 8,
           "idle keep-alive connection is not held to the header deadline");
    ::close(fine);
    server.stop();
    serving.get();
}

long cpuTicks(pid_t pid)
{
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string content((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
    const size_t close = content.rfind(')');
    std::istringstream fields(content.substr(close + 2));
    std::string skip;
    for (int index = 0; index < 11; ++index)
        fields >> skip;

    long user = 0;
    long system = 0;
    fields >> user >> system;
    return user + system;
}

// A server that runs out of descriptors must shed the queued connections
// rather than spin on the failing accept.
void descriptorExhaustionTest()
{
    http::ServerConfig config = baseConfig();
    const pid_t child = fork();
    if (child == 0) {
        rlimit limit{40, 40};
        setrlimit(RLIMIT_NOFILE, &limit);
        static std::atomic<bool> stop{false};
        signal(SIGTERM, [](int) { stop = true; });
        http::Server server(config);
        server.onGet("/ping", [](http::HttpContext& context) { context.text("pong"); });
        server.run([] { return stop.load(); });
        std::_Exit(0);
    }
    std::vector<int> clients;
    for (int index = 0; index < 120; ++index) {
        try {
            clients.push_back(connectTo(config.port, 1));
        } catch (...) {
            break;
        }
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const long before = cpuTicks(child);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const long spent = cpuTicks(child) - before;
    // 100 ticks/s: a spinning accept loop burns ~200 ticks in 2 s.
    expect(spent < 60, 9, "no busy loop while out of descriptors");
    closeAll(clients);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    expect(roundTrip(config.port, "GET /ping HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
                   .find("pong") != std::string::npos,
           10, "server recovers once descriptors are free");
    kill(child, SIGTERM);
    int status = 0;
    waitpid(child, &status, 0);
}
}  // namespace

int main()
{
    descriptorExhaustionTest();
    limitTests();
    headerDeadlineTest();
    std::puts("limits: per-loop and per-address caps, header deadline, descriptor exhaustion passed");
}
