// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// http::WebSocketClient against http::Server and a raw-socket server (for what
// Server never does: fragments, pings of its own, odd closes).

#include "http/server.hpp"
#include "http/websocket.hpp"
#include "http/websocket_client.hpp"
#include "test_support.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <future>
#include <new>
#include <thread>

using namespace std::chrono_literals;
using http::WebSocketClient;
using http::WebSocketError;
using http::WebSocketClientOptions;
using namespace testsupport;

namespace {
thread_local bool t_counting = false;
std::atomic<unsigned long> g_news{0};
}  // namespace

void* operator new(size_t size)
{
    if (t_counting)
        g_news.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

namespace {
void expect(bool condition, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::_Exit(1);
    }
}

std::string url(uint16_t port, const std::string& path = "/echo") { return "ws://127.0.0.1:" + std::to_string(port) + path; }

const auto forever = std::chrono::steady_clock::time_point::max();

struct EchoServer
{
    http::ServerConfig config;
    std::unique_ptr<http::Server> server;
    std::future<void> serving;
    std::vector<std::shared_ptr<http::WebSocketConnection>> sockets;
    EchoServer(std::function<void(http::Server&, EchoServer&)> setup = {})
    {
        config.host = "127.0.0.1";
        config.port = freePort();
        config.ioThreads = 1;
        config.workerThreads = 1;
        config.keepAliveTimeoutSeconds = 75;
        if (setup) {
            // allow tweaking config before the server exists
        }
        server = std::make_unique<http::Server>(config);
        server->onWebSocket("/echo", [this](std::shared_ptr<http::WebSocketConnection> c) {
            http::WebSocketConnection* raw = c.get();
            c->onMessage([raw](std::string_view data, bool binary) {
                if (data == "close-with-policy")
                    raw->close(1008, "policy violation");
                else
                    raw->send(data, binary);
            });
            sockets.push_back(c);
        });
        http::WebSocketRouteOptions guarded;
        guarded.subprotocols = {"proto.v1"};
        guarded.accept = [](const http::WebSocketHandshakeRequest& request) {
            http::WebSocketAccept decision;
            const auto auth = request.request.headers.get("Authorization");
            decision.status = auth == "Bearer ok" ? 101 : (auth.empty() ? 401 : 403);
            return decision;
        };
        server->onWebSocket("/guarded", [this](std::shared_ptr<http::WebSocketConnection> c) { sockets.push_back(c); }, guarded);
        serving = std::async(std::launch::async, [this] { server->run(); });
        ::close(connectTo(config.port));  // wait until it listens
    }
    ~EchoServer()
    {
        sockets.clear();
        server->stop();
        serving.get();
    }
};

void urls()
{
    auto a = http::parseWebSocketUrl("ws://example.com/db");
    expect(!a.secure && a.host == "example.com" && a.port == 80 && a.target == "/db", "plain url");
    auto b = http::parseWebSocketUrl("wss://h:8443/a/b?x=1&y=2");
    expect(b.secure && b.host == "h" && b.port == 8443 && b.target == "/a/b?x=1&y=2", "secure url with query");
    auto c = http::parseWebSocketUrl("ws://[::1]:9000");
    expect(c.host == "::1" && c.port == 9000 && c.target == "/", "ipv6 literal, no path");
    auto d = http::parseWebSocketUrl("ws://h?q=1");
    expect(d.target == "/?q=1", "query without a path");
    expect(http::parseWebSocketUrl("wss://h").port == 443, "default tls port");
    for (const char* bad : {"http://h/", "ws://", "ws://:80/", "ws://h:0/", "ws://h:99999/", "ws://h:x/", "ws://u:p@h/", "ws://[::1/", "h/"}) {
        bool threw = false;
        try {
            http::parseWebSocketUrl(bad);
        } catch (const WebSocketError& e) {
            threw = e.kind() == WebSocketError::Kind::Usage;
        }
        expect(threw, bad);
    }
}

void echoSizes()
{
    EchoServer s;
    WebSocketClient client;
    client.connect(url(s.config.port));
    expect(client.isOpen(), "open");
    for (size_t size : {size_t{0}, size_t{1}, size_t{125}, size_t{126}, size_t{65535}, size_t{65536}, size_t{1024 * 1024}}) {
        std::string data(size, '\0');
        for (size_t i = 0; i < size; ++i)
            data[i] = static_cast<char>('a' + (i * 7 + size) % 26);
        client.sendBinary(data.data(), data.size());
        auto reply = client.receive(std::chrono::steady_clock::now() + 5s);
        expect(reply && reply->binary && reply->data == data, "binary echo");
        expect(reinterpret_cast<uintptr_t>(reply->data.data()) % 8 == 0, "views are 8-aligned");
        client.sendText(std::string(size % 1000, 'x'));
        auto text = client.receive(std::chrono::steady_clock::now() + 5s);
        expect(text && !text->binary && text->data == std::string(size % 1000, 'x'), "text echo");
    }
    client.close();
    expect(!client.isOpen(), "closed");
}

void handshakeRefusals()
{
    EchoServer s;
    auto status = [&](const std::string& path, WebSocketClientOptions options) {
        WebSocketClient c;
        try {
            c.connect(url(s.config.port, path), options);
        } catch (const WebSocketError& e) {
            expect(e.kind() == WebSocketError::Kind::Handshake, "handshake error kind");
            return e.status();
        }
        return 101;
    };
    WebSocketClientOptions plain;
    expect(status("/nothing", plain) == 404, "unknown route -> 404");
    plain.subprotocols = {"proto.v1"};
    expect(status("/guarded", plain) == 401, "no credentials -> 401");
    plain.headers = {{"Authorization", "Bearer no"}};
    expect(status("/guarded", plain) == 403, "bad credentials -> 403");
    plain.headers = {{"Authorization", "Bearer ok"}};
    WebSocketClient c;
    c.connect(url(s.config.port, "/guarded"), plain);
    expect(c.subprotocol() == "proto.v1", "chosen sub-protocol");
    WebSocketClientOptions none;
    none.headers = {{"Authorization", "Bearer ok"}};
    expect(status("/guarded", none) == 400, "required sub-protocol missing -> 400");

    bool refused = false;
    try {
        WebSocketClient dead;
        dead.connect(url(freePort()));
    } catch (const WebSocketError& e) {
        refused = e.kind() == WebSocketError::Kind::Connect;
    }
    expect(refused, "nothing listening -> Connect");
    bool tls = false;
    try {
        WebSocketClient w;
        w.connect("wss://127.0.0.1:1/");
    } catch (const WebSocketError& e) {
        tls = e.kind() == WebSocketError::Kind::Tls;
    }
    expect(tls, "wss reports Tls");
}

void timeoutsAndPings()
{
    EchoServer s;
    WebSocketClient client;
    client.connect(url(s.config.port));
    const auto start = std::chrono::steady_clock::now();
    expect(!client.receive(start + 200ms), "receive times out");
    expect(std::chrono::steady_clock::now() - start >= 190ms, "waited for the deadline");
    client.sendText("still works");
    auto reply = client.receive(std::chrono::steady_clock::now() + 2s);
    expect(reply && reply->data == "still works", "usable after a timeout");
    // pingEvery sends pings while waiting; the server answers with pongs that receive() swallows.
    expect(!client.receive(std::chrono::steady_clock::now() + 400ms, 100ms), "silence with pings");
    client.ping("explicit");
    client.sendText("after pings");
    reply = client.receive(std::chrono::steady_clock::now() + 2s);
    expect(reply && reply->data == "after pings", "pongs are not messages");
}

void closeCodes()
{
    EchoServer s;
    WebSocketClient client;
    client.connect(url(s.config.port));
    client.sendText("close-with-policy");
    bool closed = false;
    try {
        client.receive(std::chrono::steady_clock::now() + 5s);
    } catch (const WebSocketError& e) {
        closed = e.kind() == WebSocketError::Kind::Closed && e.closeCode() == 1008 && e.closeReason() == "policy violation";
    }
    expect(closed, "close(code, reason) reaches the client");
}

void sendFromAnotherThread()
{
    EchoServer s;
    WebSocketClient client;
    client.connect(url(s.config.port));
    std::thread sender([&] {
        std::this_thread::sleep_for(150ms);
        client.sendText("from the other thread");
    });
    auto reply = client.receive(std::chrono::steady_clock::now() + 5s);  // blocked here while the other thread sends
    sender.join();
    expect(reply && reply->data == "from the other thread", "send while another thread receives");
}

// A raw server: accepts one connection, completes the handshake, then runs `script`.
struct RawServer
{
    uint16_t port = freePort();
    std::thread thread;
    int listener = -1;
    explicit RawServer(std::function<void(int)> script)
    {
        listener = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || ::listen(listener, 1))
            std::_Exit(2);
        thread = std::thread([this, script] {
            int fd = ::accept(listener, nullptr, nullptr);
            std::string request = readUntil(fd, "\r\n\r\n");
            const size_t at = request.find("Sec-WebSocket-Key: ");
            std::string key = request.substr(at + 19, request.find("\r\n", at) - at - 19);
            sendAll(fd, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " +
                            http::computeWebSocketAccept(key) + "\r\n\r\n");
            script(fd);
            ::close(fd);
        });
    }
    ~RawServer()
    {
        thread.join();
        ::close(listener);
    }
};

std::string unmasked(http::WsOpcode opcode, std::string_view payload, bool fin)
{
    return http::encodeFrame(opcode, payload, fin);
}

void fragmentsPingsAndCloses()
{
    {
        RawServer raw([](int fd) {
            sendAll(fd, unmasked(http::WsOpcode::Ping, "are you there", true));
            sendAll(fd, unmasked(http::WsOpcode::Text, "he", false));
            sendAll(fd, unmasked(http::WsOpcode::Ping, "mid-message", true));  // control frames may interleave
            sendAll(fd, unmasked(http::WsOpcode::Continuation, "llo ", false));
            sendAll(fd, unmasked(http::WsOpcode::Continuation, "world", true));
            // The client's pongs come back masked; just drain a little before closing.
            char sink[256];
            ::recv(fd, sink, sizeof(sink), 0);
            sendAll(fd, unmasked(http::WsOpcode::Close, std::string("\x03\xe9", 2) + "going away", true));
            ::recv(fd, sink, sizeof(sink), 0);
        });
        WebSocketClient client;
        client.connect(url(raw.port, "/"));
        auto message = client.receive(std::chrono::steady_clock::now() + 5s);
        expect(message && !message->binary && message->data == "hello world", "fragments reassembled around pings");
        bool closed = false;
        try {
            client.receive(std::chrono::steady_clock::now() + 5s);
        } catch (const WebSocketError& e) {
            closed = e.kind() == WebSocketError::Kind::Closed && e.closeCode() == 1001 && e.closeReason() == "going away";
        }
        expect(closed, "server close surfaces its code and reason");
        expect(!client.isOpen() && client.peerClosed(), "closed after the server's Close");
    }
    {
        RawServer raw([](int fd) {
            sendAll(fd, unmasked(http::WsOpcode::Binary, std::string(5000, 'x'), true));
            std::this_thread::sleep_for(300ms);
        });
        WebSocketClient client;
        http::WebSocketClientOptions options;
        options.maxMessageBytes = 1000;
        client.connect(url(raw.port, "/"), options);
        bool tooBig = false;
        try {
            client.receive(std::chrono::steady_clock::now() + 5s);
        } catch (const WebSocketError& e) {
            tooBig = e.kind() == WebSocketError::Kind::TooBig;
        }
        expect(tooBig, "a message above maxMessageBytes");
    }
    {
        RawServer raw([](int fd) {
            std::string frame = unmasked(http::WsOpcode::Text, "x", true);
            frame[1] = static_cast<char>(frame[1] | 0x80);  // a masked frame from a server
            frame.insert(2, 4, '\0');
            sendAll(fd, frame);
            std::this_thread::sleep_for(300ms);
        });
        WebSocketClient client;
        client.connect(url(raw.port, "/"));
        bool protocol = false;
        try {
            client.receive(std::chrono::steady_clock::now() + 5s);
        } catch (const WebSocketError& e) {
            protocol = e.kind() == WebSocketError::Kind::Protocol;
        }
        expect(protocol, "a masked frame from the server is a protocol error");
    }
    {
        RawServer raw([](int fd) { std::this_thread::sleep_for(100ms); (void)fd; });  // then just hangs up
        WebSocketClient client;
        client.connect(url(raw.port, "/"));
        std::this_thread::sleep_for(300ms);
        expect(client.peerClosed(), "end of stream is noticed without reading");
    }
}

void warmAllocations()
{
    EchoServer s;
    WebSocketClient client;
    client.connect(url(s.config.port));
    std::string data(64 * 1024, 'q');
    for (int i = 0; i < 20; ++i) {
        client.sendBinary(data.data(), data.size());
        client.receive(forever);
    }
    t_counting = true;
    g_news = 0;
    for (int i = 0; i < 200; ++i) {
        client.sendBinary(data.data(), data.size());
        auto reply = client.receive(forever);
        if (!reply || reply->data.size() != data.size())
            expect(false, "echo");
    }
    t_counting = false;
    std::printf("warm sendBinary + receive of 64 KiB: %lu allocations in 200 round trips\n", g_news.load());
    expect(g_news.load() == 0, "no heap allocation per message once warm");
}
}  // namespace

int main()
{
    urls();
    echoSizes();
    handshakeRefusals();
    timeoutsAndPings();
    closeCodes();
    sendFromAnotherThread();
    fragmentsPingsAndCloses();
    warmAllocations();
    std::puts("websocket client: urls, echo, refusals, timeouts, pings, fragments, closes and allocations passed");
}
