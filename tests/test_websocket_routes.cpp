// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Server::onWebSocket with WebSocketRouteOptions (sub-protocols, the accept
// hook, per-route message limit and idle timeout) and
// WebSocketConnection::send(Buffer&&): big frames written from their pooled
// blocks, in order with the copied ones, and returned to the pool.

#include "http/server.hpp"
#include "test_support.hpp"

#include <sys/socket.h>

#include <cstdlib>
#include <cstring>
#include <future>
#include <vector>

using namespace testsupport;

namespace {
void expect(bool condition, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::_Exit(1);
    }
}

// Sends a handshake and returns the response head (empty if the server hung up).
std::string handshake(int client, const std::string& path, const std::string& extraHeaders = {})
{
    sendAll(client, "GET " + path + " HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
                    "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" +
                        extraHeaders + "\r\n");
    return readUntil(client, "\r\n\r\n");
}

std::string maskedFrame(int opcode, std::string_view payload)
{
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));
    if (payload.size() < 126) {
        frame.push_back(static_cast<char>(0x80 | payload.size()));
    } else if (payload.size() < 65536) {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>(payload.size() >> 8));
        frame.push_back(static_cast<char>(payload.size()));
    } else {
        frame.push_back(static_cast<char>(0x80 | 127));
        for (int shift = 56; shift >= 0; shift -= 8)
            frame.push_back(static_cast<char>(payload.size() >> shift));
    }
    frame.append(4, '\0');  // a zero mask leaves the payload as it is
    frame.append(payload.data(), payload.size());
    return frame;
}

bool receiveExactly(int client, char* into, size_t size)
{
    while (size) {
        const ssize_t got = ::recv(client, into, size, 0);
        if (got <= 0)
            return false;
        into += got;
        size -= static_cast<size_t>(got);
    }
    return true;
}

bool readFrame(int client, int& opcode, std::string& payload)
{
    unsigned char header[2];
    if (!receiveExactly(client, reinterpret_cast<char*>(header), 2))
        return false;
    opcode = header[0] & 0x0F;
    uint64_t length = header[1] & 0x7F;
    if (length == 126) {
        unsigned char extended[2];
        if (!receiveExactly(client, reinterpret_cast<char*>(extended), 2))
            return false;
        length = static_cast<uint64_t>(extended[0]) << 8 | extended[1];
    } else if (length == 127) {
        unsigned char extended[8];
        if (!receiveExactly(client, reinterpret_cast<char*>(extended), 8))
            return false;
        length = 0;
        for (unsigned char b : extended)
            length = length << 8 | b;
    }
    payload.resize(length);
    return receiveExactly(client, payload.data(), length);
}

struct Fixture
{
    http::ServerConfig config;
    std::unique_ptr<http::Server> server;
    std::future<void> serving;
    std::vector<std::shared_ptr<http::WebSocketConnection>> sockets;

    Fixture()
    {
        config.host = "127.0.0.1";
        config.port = freePort();
        config.ioThreads = 1;
        config.workerThreads = 1;
        config.maxWebSocketQueuedBytes = 64 * 1024 * 1024;
        config.maxWebSocketMessageBytes = 16 * 1024 * 1024;
    }
    void start()
    {
        serving = std::async(std::launch::async, [this] { server->run(); });
    }
    ~Fixture()
    {
        if (server) {
            server->stop();
            serving.get();
        }
    }
};

void subprotocols()
{
    Fixture f;
    f.server = std::make_unique<http::Server>(f.config);
    http::WebSocketRouteOptions options;
    options.subprotocols = {"libsql.v1", "other.v2"};
    f.server->onWebSocket("/db", [&](std::shared_ptr<http::WebSocketConnection> c) { f.sockets.push_back(c); }, options);
    f.start();

    int a = connectTo(f.config.port);
    const std::string offered = handshake(a, "/db", "Sec-WebSocket-Protocol: nope, other.v2 , libsql.v1\r\n");
    expect(offered.rfind("HTTP/1.1 101", 0) == 0, "an offered, listed sub-protocol upgrades");
    // The first of the client's offers that the route lists wins.
    expect(offered.find("Sec-WebSocket-Protocol: other.v2\r\n") != std::string::npos, "chosen sub-protocol is echoed");
    ::close(a);

    int b = connectTo(f.config.port);
    expect(handshake(b, "/db").rfind("HTTP/1.1 400", 0) == 0, "no sub-protocol offered is refused");
    ::close(b);
    int c = connectTo(f.config.port);
    expect(handshake(c, "/db", "Sec-WebSocket-Protocol: nope\r\n").rfind("HTTP/1.1 400", 0) == 0,
           "an unlisted sub-protocol is refused");
    ::close(c);
}

void acceptHook()
{
    Fixture f;
    f.server = std::make_unique<http::Server>(f.config);
    http::WebSocketRouteOptions options;
    options.accept = [](const http::WebSocketHandshakeRequest& request) {
        http::WebSocketAccept decision;
        const std::string_view auth = request.request.headers.get("Authorization");
        if (auth.empty())
            decision.status = 401;
        else if (auth != "Bearer good")
            decision.status = 403;
        else
            decision.userData = std::make_shared<std::string>("user:" + std::string(request.routeParams.empty() ? "" : request.routeParams[0].second));
        return decision;
    };
    std::string seen;
    f.server->onWebSocket("/db/{tenant}", [&](std::shared_ptr<http::WebSocketConnection> c) {
        auto data = std::static_pointer_cast<std::string>(c->userData());
        seen = data ? *data : "(null)";
        f.sockets.push_back(c);
    }, options);
    f.start();

    int a = connectTo(f.config.port);
    expect(handshake(a, "/db/acme").rfind("HTTP/1.1 401", 0) == 0, "no credentials -> 401");
    ::close(a);
    int b = connectTo(f.config.port);
    expect(handshake(b, "/db/acme", "Authorization: Bearer bad\r\n").rfind("HTTP/1.1 403", 0) == 0, "bad credentials -> 403");
    ::close(b);
    int c = connectTo(f.config.port);
    expect(handshake(c, "/db/acme", "Authorization: Bearer good\r\n").rfind("HTTP/1.1 101", 0) == 0, "good credentials upgrade");
    // The handler ran after the 101; give it a moment through a round trip.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    expect(seen == "user:acme", "userData reaches the handler");
    ::close(c);
}

void perRouteLimits()
{
    Fixture f;
    f.server = std::make_unique<http::Server>(f.config);
    http::WebSocketRouteOptions small;
    small.maxMessageBytes = 1000;
    small.idleTimeoutSeconds = 1;
    for (const char* path : {"/small", "/big"}) {
        const http::WebSocketRouteOptions options = std::string_view(path) == "/small" ? small : http::WebSocketRouteOptions{};
        f.server->onWebSocket(path, [&](std::shared_ptr<http::WebSocketConnection> c) {
            http::WebSocketConnection* raw = c.get();
            c->onMessage([raw](std::string_view data, bool) { raw->send(std::string_view("ok:") , false); (void)data; });
            f.sockets.push_back(c);
        }, options);
    }
    f.start();

    // A frame above the route's limit closes with 1009 on that route...
    int a = connectTo(f.config.port);
    expect(handshake(a, "/small").rfind("HTTP/1.1 101", 0) == 0, "small route upgrades");
    sendAll(a, maskedFrame(2, std::string(2000, 'x')));
    int opcode = 0;
    std::string payload;
    expect(readFrame(a, opcode, payload) && opcode == 8 && payload.size() >= 2 &&
               static_cast<unsigned char>(payload[0]) == 0x03 && static_cast<unsigned char>(payload[1]) == 0xF1,
           "oversize frame closes with 1009");
    ::close(a);
    // ...while another route takes the same frame.
    int b = connectTo(f.config.port);
    expect(handshake(b, "/big").rfind("HTTP/1.1 101", 0) == 0, "big route upgrades");
    sendAll(b, maskedFrame(2, std::string(2000, 'x')));
    expect(readFrame(b, opcode, payload) && opcode == 1 && payload == "ok:", "the other route answers");
    ::close(b);

    // The route's idle timeout (1 s) closes a silent connection; a ping keeps one open.
    int quiet = connectTo(f.config.port, 6);
    handshake(quiet, "/small");
    int busy = connectTo(f.config.port, 6);
    handshake(busy, "/small");
    const auto started = std::chrono::steady_clock::now();
    for (int i = 0; i < 4; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        sendAll(busy, maskedFrame(9, "k"));
        expect(readFrame(busy, opcode, payload) && opcode == 10, "pong for the ping");
    }
    char byte;
    const ssize_t got = ::recv(quiet, &byte, 1, 0);
    expect(got <= 0 || (static_cast<unsigned char>(byte) & 0x0F) == 8, "the silent connection was closed");
    expect(std::chrono::steady_clock::now() - started < std::chrono::seconds(4), "closed by the route's 1 s timeout, not the server's");
    sendAll(busy, maskedFrame(1, "still"));
    expect(readFrame(busy, opcode, payload) && opcode == 1, "the pinging connection stays open");
    ::close(quiet);
    ::close(busy);
}

void pooledSend()
{
    Fixture f;
    f.config.maxWebSocketQueuedFrames = 4096;
    f.server = std::make_unique<http::Server>(f.config);
    constexpr int kFrames = 200;
    constexpr size_t kBig = 256 * 1024;
    f.server->onWebSocket("/blob", [&](std::shared_ptr<http::WebSocketConnection> c) {
        http::WebSocketConnection* raw = c.get();
        c->onMessage([raw, &f](std::string_view, bool) {
            for (int i = 0; i < kFrames; ++i) {
                http::Buffer frame(f.server->bufferPool(), kBig);
                std::memset(frame.data(), 'a' + i % 26, kBig);
                frame.setSize(kBig);
                raw->send(std::move(frame), true);
                if (i % 10 == 0)
                    raw->send("small " + std::to_string(i), false);  // copied frames keep their place
            }
        });
        f.sockets.push_back(c);
    });
    f.start();

    const auto before = f.server->bufferPool().stats();
    int client = connectTo(f.config.port, 20);
    handshake(client, "/blob");
    sendAll(client, maskedFrame(1, "go"));
    // A slow reader: read in pieces with pauses, so the server has to queue.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    int opcode = 0;
    std::string payload;
    int big = 0, small = 0;
    while (big < kFrames || small < kFrames / 10) {
        expect(readFrame(client, opcode, payload), "frame arrives");
        if (opcode == 2) {
            expect(payload.size() == kBig && payload[0] == static_cast<char>('a' + big % 26) &&
                       payload[kBig - 1] == payload[0],
                   "big frame intact and in order");
            ++big;
        } else {
            expect(opcode == 1 && payload == "small " + std::to_string(small * 10), "small frame in its place");
            expect(big == small * 10 + 1, "small frame sits between the right big ones");
            ++small;
        }
    }
    ::close(client);
    f.sockets.clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto after = f.server->bufferPool().stats();
    expect(after.outstandingBlocks <= before.outstandingBlocks + 4, "blocks went back to the pool");
}

void queueLimitCountsPooledFrames()
{
    Fixture f;
    f.config.maxWebSocketQueuedBytes = 1024 * 1024;
    f.server = std::make_unique<http::Server>(f.config);
    f.server->onWebSocket("/flood", [&](std::shared_ptr<http::WebSocketConnection> c) {
        http::WebSocketConnection* raw = c.get();
        c->onMessage([raw, &f](std::string_view, bool) {
            for (int i = 0; i < 100; ++i) {
                http::Buffer frame(f.server->bufferPool(), 256 * 1024);
                frame.setSize(256 * 1024);
                std::memset(frame.data(), 'z', frame.size());
                raw->send(std::move(frame), true);
            }
        });
        f.sockets.push_back(c);
    });
    f.start();
    int client = connectTo(f.config.port);
    handshake(client, "/flood");
    sendAll(client, maskedFrame(1, "go"));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    int opcode = 0;
    std::string payload;
    bool closed = false;
    for (int i = 0; i < 200 && !closed; ++i)
        closed = !readFrame(client, opcode, payload) || opcode == 8;
    expect(closed, "a client this slow is cut off at the byte limit");
    ::close(client);
}
}  // namespace

int main()
{
    subprotocols();
    acceptHook();
    perRouteLimits();
    pooledSend();
    queueLimitCountsPooledFrames();
    std::puts("websocket routes: sub-protocols, accept hook, per-route limits and pooled sends passed");
}
