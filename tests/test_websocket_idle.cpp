// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// What lets a server hold very many WebSocket connections cheaply, and must not change what they carry:
//   - the idle sweep (ServerConfig::idleTrimSeconds) gives an idle connection's buffers back, and the connection works
//     as before afterwards, a frame left half read across a sweep included;
//   - the queue of pooled frames grows on demand and keeps their order;
//   - over TLS, a send from one connection's handler to another connection of the same loop, while both read, keeps
//     every byte of both (they share the loop's buffers).

#include "http/server.hpp"
#include "http/websocket_client.hpp"
#include "test_support.hpp"
#ifdef HTTP_WITH_SSL
#include "tls_pki.hpp"
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace testsupport;

namespace {

void expect(bool condition, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::_Exit(1);
    }
}

const auto forever = std::chrono::steady_clock::time_point::max();

struct Fixture
{
    http::ServerConfig config;
    std::unique_ptr<http::Server> server;
    std::future<void> serving;

    explicit Fixture(http::ServerConfig base)
        : config(std::move(base))
    {
        config.host = "127.0.0.1";
        config.port = freePort();
        server = std::make_unique<http::Server>(config);
    }
    void start(uint16_t probePort)
    {
        serving = std::async(std::launch::async, [this] { server->run(); });
        ::close(connectTo(probePort));
    }
    ~Fixture()
    {
        server->stop();
        serving.get();
    }
};

std::string pattern(size_t size, unsigned seed)
{
    std::string out(size, '\0');
    for (size_t i = 0; i < size; ++i)
        out[i] = static_cast<char>((i * 131 + seed * 7) & 0xff);
    return out;
}

// A masked client frame, by hand, so that a test can stop in the middle of one.
std::string clientFrame(const std::string& payload)
{
    std::string frame;
    frame.push_back(static_cast<char>(0x82));
    if (payload.size() < 126) {
        frame.push_back(static_cast<char>(0x80 | payload.size()));
    } else {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>(payload.size() >> 8));
        frame.push_back(static_cast<char>(payload.size() & 0xff));
    }
    const unsigned char mask[4] = {0x11, 0x22, 0x33, 0x44};
    frame.append(reinterpret_cast<const char*>(mask), 4);
    for (size_t i = 0; i < payload.size(); ++i)
        frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
    return frame;
}

void sendAll(int socket, const std::string& bytes)
{
    size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t n = ::send(socket, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        expect(n > 0, "send");
        sent += static_cast<size_t>(n);
    }
}

// Reads one unmasked server frame's payload (no fragmentation, payload < 64 KiB).
std::string readServerFrame(int socket)
{
    const auto readExactly = [socket](size_t size) {
        std::string out(size, '\0');
        size_t got = 0;
        while (got < size) {
            const ssize_t n = ::recv(socket, out.data() + got, size - got, 0);
            expect(n > 0, "recv");
            got += static_cast<size_t>(n);
        }
        return out;
    };
    const std::string head = readExactly(2);
    size_t length = static_cast<unsigned char>(head[1]) & 0x7f;
    if (length == 126) {
        const std::string extended = readExactly(2);
        length = (static_cast<unsigned char>(extended[0]) << 8) | static_cast<unsigned char>(extended[1]);
    }
    return readExactly(length);
}

void idleSweepKeepsConnectionsWorking()
{
    http::ServerConfig config;
    config.ioThreads = 1;
    config.idleTrimSeconds = 1;
    Fixture f(config);
    f.server->onWebSocket("/echo", [](std::shared_ptr<http::WebSocketConnection> c) {
        http::WebSocketConnection* raw = c.get();
        c->onMessage([raw](std::string_view data, bool binary) { raw->send(data, binary); });
    });
    f.start(f.config.port);
    const std::string url = "ws://127.0.0.1:" + std::to_string(f.config.port) + "/echo";

    http::WebSocketClient client;
    client.connect(url);
    for (int round = 0; round < 3; ++round) {
        for (const size_t size : {size_t{5}, size_t{3000}, size_t{70000}, size_t{300000}}) {
            const std::string message = pattern(size, static_cast<unsigned>(round));
            client.sendBinary(message.data(), message.size());
            const auto reply = client.receive(forever);
            expect(reply && reply->data == message, "echo intact around idle sweeps");
        }
        std::this_thread::sleep_for(2500ms);  // two sweeps: the first sees it busy, the second trims
    }
    client.close();

    // A frame half sent when the sweeps run: the half already read is kept, and the rest completes it.
    const int socket = connectTo(f.config.port);
    sendAll(socket, "GET /echo HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    std::string response;
    char c;
    while (response.find("\r\n\r\n") == std::string::npos && ::recv(socket, &c, 1, 0) == 1)
        response.push_back(c);
    expect(response.rfind("HTTP/1.1 101", 0) == 0, "upgraded");
    const std::string message = pattern(20000, 9);
    const std::string frame = clientFrame(message);
    sendAll(socket, frame.substr(0, 7000));
    std::this_thread::sleep_for(2500ms);
    sendAll(socket, frame.substr(7000));
    expect(readServerFrame(socket) == message, "a frame split across idle sweeps arrives whole");
    ::close(socket);
}

void pooledFrameQueueGrowsInOrder()
{
    http::ServerConfig config;
    config.ioThreads = 1;
    config.maxWebSocketQueuedFrames = 4096;
    config.maxWebSocketQueuedBytes = 64 * 1024 * 1024;
    Fixture f(config);
    constexpr int kFrames = 300;
    constexpr size_t kBytes = 16 * 1024;  // above the size sent by copy, so each one waits in the queue
    http::Server* server = f.server.get();
    f.server->onWebSocket("/burst", [server](std::shared_ptr<http::WebSocketConnection> c) {
        http::WebSocketConnection* raw = c.get();
        c->onMessage([raw, server](std::string_view, bool) {
            for (int i = 0; i < kFrames; ++i) {
                http::Buffer frame(server->bufferPool(), kBytes);
                const std::string body = pattern(kBytes, static_cast<unsigned>(i));
                frame.append(body);
                raw->send(std::move(frame), true);
            }
        });
    });
    f.start(f.config.port);
    http::WebSocketClient client;
    client.connect("ws://127.0.0.1:" + std::to_string(f.config.port) + "/burst");
    client.sendText("go");
    std::this_thread::sleep_for(200ms);  // let the queue fill before reading
    for (int i = 0; i < kFrames; ++i) {
        const auto frame = client.receive(forever);
        expect(frame && frame->data == pattern(kBytes, static_cast<unsigned>(i)), "pooled frames arrive whole and in order");
    }
    client.close();
}

#ifdef HTTP_WITH_SSL
// One loop. A message from `a` is forwarded to `b` from inside a's handler, while b's client floods it with small
// messages to echo. The forward pumps b's TLS stream inside a's callback, while a's message is still a view into a buffer
// the loop shares: the loop's receive buffer when `a` is plain, the per-thread plaintext buffer when it is TLS. b's
// stream then has data to read, and must not read it into either. The handler checks its view is unchanged after the
// send.
void tlsSendFromAnotherConnectionsHandlerKeepsBytes(bool aOverTls)
{
    Pki pki;
    http::ServerConfig config;
    config.ioThreads = 1;
    // What the flood below can have queued for b's client; the default limits would rightly cut it off.
    config.maxWebSocketQueuedFrames = 1 << 20;
    config.maxWebSocketQueuedBytes = 256 * 1024 * 1024;
    Fixture f(config);
    f.config.tls = http::TlsConfig{freePort(), pki.certificate, pki.key};
    f.server = std::make_unique<http::Server>(f.config);
    std::shared_ptr<http::WebSocketConnection> b;  // set and read on the loop only
    std::atomic<bool> bReady{false};
    std::atomic<int> corrupted{0};
    f.server->onWebSocket("/b", [&](std::shared_ptr<http::WebSocketConnection> c) {
        http::WebSocketConnection* raw = c.get();
        c->onMessage([raw](std::string_view data, bool binary) { raw->send(data, binary); });
        b = c;
        bReady = true;
    });
    f.server->onWebSocket("/a", [&](std::shared_ptr<http::WebSocketConnection> c) {
        c->onMessage([&](std::string_view data, bool binary) {
            const std::string before(data);
            if (b)
                b->send(data, binary);
            if (data != before)
                ++corrupted;
        });
    });
    f.start(f.config.tls->port);

    http::WebSocketClientOptions options;
    options.caFile = pki.certificate;
    const std::string tlsBase = "wss://127.0.0.1:" + std::to_string(f.config.tls->port);
    const std::string plainBase = "ws://127.0.0.1:" + std::to_string(f.config.port);
    http::WebSocketClient clientB;
    clientB.connect(tlsBase + "/b", options);
    for (int i = 0; i < 500 && !bReady; ++i)
        std::this_thread::sleep_for(10ms);
    http::WebSocketClient clientA;
    clientA.connect((aOverTls ? tlsBase : plainBase) + "/a", options);

    constexpr int kForwarded = 4000;
    std::atomic<bool> done{false};
    std::atomic<int> echoesSent{0};
    std::atomic<bool> sendFailed{false};
    std::thread flooding([&] {
        const std::string small(40, 'b');
        try {
            for (int i = 0; i < 50000 && !done; ++i) {
                clientB.sendBinary(small.data(), small.size());
                ++echoesSent;
            }
        } catch (const http::WebSocketError&) {
            sendFailed = true;
        }
    });
    std::thread forwarding([&] {
        try {
            for (int i = 0; i < kForwarded; ++i) {
                const std::string message = "A" + pattern(200 + (i % 50) * 31, static_cast<unsigned>(i));
                clientA.sendBinary(message.data(), message.size());
            }
        } catch (const http::WebSocketError&) {
            sendFailed = true;
        }
    });
    int fromA = 0, fromB = 0;
    bool intact = true;
    while (fromA < kForwarded) {
        std::optional<http::WebSocketClient::Message> message;
        try {
            message = clientB.receive(std::chrono::steady_clock::now() + 20s);
        } catch (const http::WebSocketError& error) {
            std::fprintf(stderr, "receive: %s\n", error.what());
        }
        expect(message.has_value(), "every message arrives");
        if (!message->data.empty() && message->data[0] == 'A') {
            intact = intact && message->data == "A" + pattern(200 + (fromA % 50) * 31, static_cast<unsigned>(fromA));
            ++fromA;
        } else {
            intact = intact && message->data == std::string(40, 'b');
            ++fromB;
        }
    }
    done = true;
    forwarding.join();
    flooding.join();
    expect(!sendFailed, "the clients' sends all went through");
    expect(corrupted == 0, "a handler's view survives a send to another TLS connection of its loop");
    expect(intact, "both connections' bytes intact");
    expect(fromB > 0, "the other connection was busy meanwhile");
    clientA.close();
    clientB.close();
}
#endif

}  // namespace

int main()
{
    idleSweepKeepsConnectionsWorking();
    std::puts("ok idle_sweep_keeps_connections_working");
    pooledFrameQueueGrowsInOrder();
    std::puts("ok pooled_frame_queue_grows_in_order");
#ifdef HTTP_WITH_SSL
    // A race: rounds enough that a missing guard shows nearly every run.
    for (int round = 0; round < 6; ++round)
        tlsSendFromAnotherConnectionsHandlerKeepsBytes(false);
    std::puts("ok plain_handler_sending_to_tls_connection_keeps_its_bytes");
    for (int round = 0; round < 6; ++round)
        tlsSendFromAnotherConnectionsHandlerKeepsBytes(true);
    std::puts("ok tls_handler_sending_to_tls_connection_keeps_its_bytes");
#endif
    return 0;
}
