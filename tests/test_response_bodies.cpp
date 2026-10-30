// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Response bodies in pooled Buffers (HttpContext::bytes(Buffer&&)), large
// bodies written together with their head, pipelined, and the per-request
// state of the one HttpContext a connection reuses.

#include "http/http_context.hpp"
#include "http/server.hpp"
#include "test_support.hpp"

#include <cstdlib>
#include <cstring>
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

// Bytes received past the response just read: the start of the next one.
std::string g_pending;

// Reads one response with a Content-Length (or none, for HEAD).
std::string readResponse(int client, std::string& body, bool head = false)
{
    char bytes[65536];
    while (g_pending.find("\r\n\r\n") == std::string::npos) {
        const ssize_t got = ::recv(client, bytes, sizeof bytes, 0);
        expect(got > 0, 90, "response head");
        g_pending.append(bytes, static_cast<size_t>(got));
    }
    const size_t end = g_pending.find("\r\n\r\n") + 4;
    const std::string responseHead = g_pending.substr(0, end);
    const size_t field = responseHead.find("Content-Length: ");
    const size_t length = head || field == std::string::npos
                              ? 0
                              : std::strtoul(responseHead.c_str() + field + 16, nullptr, 10);
    while (g_pending.size() < end + length) {
        const ssize_t got = ::recv(client, bytes, sizeof bytes, 0);
        expect(got > 0, 91, "response body");
        g_pending.append(bytes, static_cast<size_t>(got));
    }
    body = g_pending.substr(end, length);
    g_pending.erase(0, end + length);
    return responseHead;
}

http::Buffer filled(http::BufferPool& pool, size_t size, char value)
{
    http::Buffer buffer(pool, size);
    std::memset(buffer.data(), value, size);
    buffer.setSize(size);
    return buffer;
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
    server.onGet("/pooled/{size}", [&](http::HttpContext& context) {
        const size_t size =
            std::strtoul(std::string(context.routeParam("size")).c_str(), nullptr, 10);
        context.bytes(filled(server.bufferPool(), size, 'p'), "text/plain");
    });
    server.onHead("/pooled/{size}", [&](http::HttpContext& context) {
        context.bytes(filled(server.bufferPool(), 300000, 'p'), "text/plain");
    });
    server.onGet("/worker", [&](http::HttpContext& context, std::function<void()> done) {
        server.pool().enqueue([&, done] {
            http::Buffer body = filled(server.bufferPool(), 500000, 'w');
            context.loop().post([&, done, body = std::make_shared<http::Buffer>(std::move(body))] {
                context.bytes(std::move(*body), "application/octet-stream");
                done();
            });
        });
    });
    server.onGet("/replaced", [&](http::HttpContext& context) {
        context.bytes(filled(server.bufferPool(), 100, 'x'));
        context.text("text wins");
    });
    server.onGet("/large/{index}", [](http::HttpContext& context) {
        std::string body(
            100000, static_cast<char>(
                        'a' + std::atoi(std::string(context.routeParam("index")).c_str()) % 26));
        context.text(body);
    });
    server.onGet("/query", [](http::HttpContext& context) {
        std::string reply = "a=" + std::string(context.query("a")) +
                            ";c=" + std::string(context.query("c")) +
                            ";hasA=" + (context.hasQuery("a") ? "1" : "0") +
                            ";cookie=" + std::string(context.cookie("session")) +
                            ";n=" + std::to_string(context.queryParams().size()) +
                            ";peer=" + context.remoteAddress();
        context.text(reply);
    });
    auto serving = std::async(std::launch::async, [&] { server.run(); });

    const int client = connectTo(config.port);
    std::string body;
    sendAll(client, "GET /query HTTP/1.1\r\nHost: x\r\n\r\n");
    readResponse(client, body);
    const size_t blocksBefore = server.bufferStats().outstandingBlocks;
    for (const size_t size : {size_t{1000}, size_t{65536}, size_t{300000}, size_t{3000000}}) {
        sendAll(client, "GET /pooled/" + std::to_string(size) + " HTTP/1.1\r\nHost: x\r\n\r\n");
        const std::string head = readResponse(client, body);
        expect(head.find("Content-Type: text/plain") != std::string::npos, 1, "content type");
        expect(body == std::string(size, 'p'), 2, "pooled body");
    }
    sendAll(client, "HEAD /pooled/300000 HTTP/1.1\r\nHost: x\r\n\r\n");
    const std::string headOnly = readResponse(client, body, true);
    expect(headOnly.find("Content-Length: 300000") != std::string::npos && body.empty(), 3,
           "HEAD carries the length only");
    sendAll(client, "GET /worker HTTP/1.1\r\nHost: x\r\n\r\n");
    readResponse(client, body);
    expect(body == std::string(500000, 'w'), 4, "pooled body from a worker");
    sendAll(client, "GET /replaced HTTP/1.1\r\nHost: x\r\n\r\n");
    readResponse(client, body);
    expect(body == "text wins", 5, "the last body set wins");
    // Every pooled body went back to the pool once written (or replaced).
    expect(server.bufferStats().outstandingBlocks == blocksBefore, 9, "pooled bodies returned");

    // Pipelined: every large body goes out behind its own head, in order.
    std::string burst;
    for (int index = 0; index < 64; ++index)
        burst += "GET /large/" + std::to_string(index) + " HTTP/1.1\r\nHost: x\r\n\r\n";
    sendAll(client, burst);
    for (int index = 0; index < 64; ++index) {
        readResponse(client, body);
        expect(body == std::string(100000, static_cast<char>('a' + index % 26)), 6,
               "pipelined large body");
    }

    // One context serves the connection's requests: nothing of one is seen by the next.
    sendAll(client,
            "GET /query?a=first%20value&b=2 HTTP/1.1\r\nHost: x\r\nCookie: session=abc\r\n\r\n");
    readResponse(client, body);
    expect(body.rfind("a=first value;c=;hasA=1;cookie=abc;n=2;peer=127.0.0.1:", 0) == 0, 7,
           "first query");
    sendAll(client, "GET /query?c=3 HTTP/1.1\r\nHost: x\r\n\r\n");
    readResponse(client, body);
    expect(body.rfind("a=;c=3;hasA=0;cookie=;n=1;peer=127.0.0.1:", 0) == 0, 8,
           "second query sees nothing of the first");
    ::close(client);

    server.stop();
    serving.get();
    std::puts(
        "response bodies: pooled bodies, HEAD, worker answers, pipelined large bodies and "
        "per-request context state passed");
}
