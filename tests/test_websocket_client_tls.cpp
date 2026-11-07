// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// http::WebSocketClient over wss:// against http::Server's TLS listener: verification
// (trusted CA, host name, IP literal, untrusted certificate), echo of small and large
// messages, a send racing a receive, pings, and the server's close.

#include "http/server.hpp"
#include "http/websocket_client.hpp"
#include "test_support.hpp"
#include "tls_pki.hpp"

#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

using namespace std::chrono_literals;
using http::WebSocketClient;
using http::WebSocketClientOptions;
using http::WebSocketError;
using namespace testsupport;
using testsupport::Pki;

namespace {
void expect(bool condition, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::_Exit(1);
    }
}

const auto forever = std::chrono::steady_clock::time_point::max();

struct TlsEcho
{
    http::ServerConfig config;
    std::unique_ptr<http::Server> server;
    std::future<void> serving;
    std::mutex socketsMutex;
    std::vector<std::shared_ptr<http::WebSocketConnection>> sockets;
    explicit TlsEcho(const Pki& pki)
    {
        config.host = "127.0.0.1";
        config.port = freePort();
        config.ioThreads = 1;
        config.workerThreads = 1;
        config.tls = http::TlsConfig{freePort(), pki.certificate, pki.key};
        while (config.tls->port == config.port)
            config.tls->port = freePort();
        server = std::make_unique<http::Server>(config);
        server->onWebSocket("/echo", [this](std::shared_ptr<http::WebSocketConnection> c) {
            http::WebSocketConnection* raw = c.get();
            c->onMessage([raw](std::string_view data, bool binary) {
                if (data == "close-with-policy")
                    raw->close(1008, "policy violation");
                else if (data == "close-bye")
                    raw->close(1000, "bye");
                else
                    raw->send(data, binary);
            });
            std::lock_guard<std::mutex> lock(socketsMutex);
            sockets.push_back(c);
        });
        serving = std::async(std::launch::async, [this] { server->run(); });
        ::close(connectTo(config.port));
    }
    ~TlsEcho()
    {
        server->stop();
        sockets.clear();
        serving.get();
    }
    std::string url(const char* host = "localhost") const { return std::string("wss://") + host + ":" + std::to_string(config.tls->port) + "/echo"; }
};

WebSocketError::Kind connectKind(const std::string& url, const WebSocketClientOptions& options, bool* ok = nullptr)
{
    WebSocketClient client;
    try {
        client.connect(url, options);
        if (ok)
            *ok = true;
    } catch (const WebSocketError& e) {
        if (ok)
            *ok = false;
        return e.kind();
    }
    return WebSocketError::Kind::Usage;
}
}  // namespace

int main()
{
    Pki pki;
    TlsEcho s(pki);
    WebSocketClientOptions trusted;
    trusted.caFile = pki.certificate;

    {  // verification
        bool ok = false;
        connectKind(s.url("localhost"), trusted, &ok);
        expect(ok, "trusted certificate, host name matches");
        connectKind(s.url("127.0.0.1"), trusted, &ok);
        expect(ok, "trusted certificate, IP literal matches");
        expect(connectKind(s.url("localhost"), {}, &ok) == WebSocketError::Kind::Tls && !ok, "a self-signed certificate is refused by default");
        WebSocketClientOptions wrong;
        wrong.caFile = pki.otherCertificate;
        expect(connectKind(s.url("localhost"), wrong, &ok) == WebSocketError::Kind::Tls && !ok, "a CA that did not sign it is refused");
        WebSocketClientOptions insecure;
        insecure.insecureSkipVerify = true;
        connectKind(s.url("localhost"), insecure, &ok);
        expect(ok, "insecureSkipVerify connects");
        // plain ws:// to the TLS port is a handshake failure, not a hang
        expect(connectKind("ws://127.0.0.1:" + std::to_string(s.config.tls->port) + "/echo", trusted, &ok) != WebSocketError::Kind::Usage || !ok, "plain to TLS fails");
    }
    {  // echo of several sizes, then the pieces of a connection's life
        WebSocketClient client;
        client.connect(s.url(), trusted);
        expect(client.isOpen(), "open");
        for (size_t size : {size_t{0}, size_t{1}, size_t{125}, size_t{126}, size_t{65535}, size_t{65536}, size_t{1024 * 1024}}) {
            std::string data(size, '\0');
            for (size_t i = 0; i < size; ++i)
                data[i] = static_cast<char>('a' + (i * 7) % 26);
            client.sendBinary(data.data(), data.size());
            auto reply = client.receive(std::chrono::steady_clock::now() + 10s);
            expect(reply && reply->binary && reply->data == data, "echo");
        }
        expect(!client.receive(std::chrono::steady_clock::now() + 300ms, 100ms), "silence with pings");
        client.sendText("after pings");
        auto reply = client.receive(std::chrono::steady_clock::now() + 5s);
        expect(reply && !reply->binary && reply->data == "after pings", "usable after pings");
        // a send from another thread while this one is blocked in receive()
        auto sender = std::async(std::launch::async, [&] {
            std::this_thread::sleep_for(100ms);
            client.sendText("from the other thread");
        });
        reply = client.receive(std::chrono::steady_clock::now() + 5s);
        sender.get();
        expect(reply && reply->data == "from the other thread", "send while another thread receives");
        // many messages, in both directions at once
        auto flood = std::async(std::launch::async, [&] {
            std::string chunk(20000, 'x');
            for (int i = 0; i < 300; ++i)
                client.sendBinary(chunk.data(), chunk.size());
        });
        for (int i = 0; i < 300; ++i) {
            auto got = client.receive(std::chrono::steady_clock::now() + 10s);
            expect(got && got->data.size() == 20000, "flood echo");
        }
        flood.get();
        client.sendText("close-with-policy");
        bool closed = false;
        try {
            client.receive(std::chrono::steady_clock::now() + 5s);
        } catch (const WebSocketError& e) {
            closed = e.kind() == WebSocketError::Kind::Closed && e.closeCode() == 1008 && e.closeReason() == "policy violation";
        }
        expect(closed, "the server's close code and reason arrive over TLS");
        expect(!client.isOpen() && client.peerClosed(), "closed afterwards");
    }
    {  // peerClosed notices an end of stream without a read
        WebSocketClient client;
        client.connect(s.url(), trusted);
        client.sendText("close-bye");
        const auto until = std::chrono::steady_clock::now() + 3s;
        while (!client.peerClosed() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(10ms);
        expect(client.peerClosed(), "peerClosed over TLS");
    }
    std::puts("== websocket_client_tls: all passed ==");
    return 0;
}
