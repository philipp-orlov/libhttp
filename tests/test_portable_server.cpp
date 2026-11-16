// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The server end to end over real sockets, written against sys/net.hpp so that it builds and runs on every platform
// (the other socket-level tests are POSIX). It goes through the paths that differ by backend: reads that land in the
// loop's buffer and reads straight into a body block, responses the socket takes at once and ones it does not,
// WebSocket frames small and large in both directions, connections dealt to several loops, and a port that is taken.

#include "http/server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "http/websocket.hpp"
#include "sys/net.hpp"

namespace {

const sys::Pal networking;

void expect(bool condition, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::_Exit(1);
    }
}

uint16_t freePort()
{
    const sys::socket_t reservation = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    expect(::bind(reservation, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind");
    socklen_t size = sizeof(address);
    getsockname(reservation, reinterpret_cast<sockaddr*>(&address), &size);
    sys::closeSocket(reservation);
    return ntohs(address.sin_port);
}

sys::socket_t connectTo(uint16_t port)
{
    for (int attempt = 0; attempt < 2000; ++attempt) {
        const sys::socket_t socket = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
#ifdef _WIN32
            const DWORD timeout = 10000;
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
#else
            timeval timeout{10, 0};
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
            return socket;
        }
        sys::closeSocket(socket);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    expect(false, "server did not start");
    return sys::kInvalidSocket;
}

void sendAll(sys::socket_t socket, std::string_view bytes)
{
    size_t sent = 0;
    while (sent < bytes.size()) {
#ifdef _WIN32
        const int n = ::send(socket, bytes.data() + sent, static_cast<int>(std::min<size_t>(bytes.size() - sent, 1 << 20)), 0);
#else
        const auto n = ::send(socket, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
#endif
        expect(n > 0, "send");
        sent += static_cast<size_t>(n);
    }
}

// Reads until `out` holds `size` bytes, or the peer closed or went quiet.
void readUpTo(sys::socket_t socket, std::string& out, size_t size)
{
    char buffer[65536];
    while (out.size() < size) {
        const auto n = ::recv(socket, buffer, static_cast<int>(std::min(sizeof buffer, size - out.size())), 0);
        if (n <= 0)
            break;
        out.append(buffer, static_cast<size_t>(n));
    }
}

// One response with a Content-Length off the connection: its head and its body.
std::pair<std::string, std::string> readResponse(sys::socket_t socket, std::string& pending)
{
    size_t end;
    while ((end = pending.find("\r\n\r\n")) == std::string::npos) {
        const size_t before = pending.size();
        readUpTo(socket, pending, pending.size() + 1);
        expect(pending.size() > before, "a response head arrives");
    }
    std::string head = pending.substr(0, end + 4);
    pending.erase(0, end + 4);
    const size_t at = head.find("Content-Length: ");
    expect(at != std::string::npos, "the response has a Content-Length");
    const size_t length = static_cast<size_t>(std::strtoull(head.c_str() + at + 16, nullptr, 10));
    readUpTo(socket, pending, length);
    expect(pending.size() >= length, "the whole response body arrives");
    std::string body = pending.substr(0, length);
    pending.erase(0, length);
    return {std::move(head), std::move(body)};
}

std::string pattern(size_t size, unsigned seed)
{
    std::string out(size, '\0');
    for (size_t i = 0; i < size; ++i)
        out[i] = static_cast<char>('a' + (i * 31 + seed * 7 + (i >> 9)) % 26);
    return out;
}

// A masked client frame.
std::string clientFrame(http::WsOpcode opcode, std::string_view payload)
{
    std::string out;
    out.push_back(static_cast<char>(0x80 | static_cast<int>(opcode)));
    const uint8_t mask[4] = {0x11, 0x22, 0x33, 0x44};
    if (payload.size() < 126) {
        out.push_back(static_cast<char>(0x80 | payload.size()));
    } else if (payload.size() <= 0xFFFF) {
        out.push_back(static_cast<char>(0x80 | 126));
        out.push_back(static_cast<char>(payload.size() >> 8));
        out.push_back(static_cast<char>(payload.size() & 0xFF));
    } else {
        out.push_back(static_cast<char>(0x80 | 127));
        for (int shift = 56; shift >= 0; shift -= 8)
            out.push_back(static_cast<char>((static_cast<uint64_t>(payload.size()) >> shift) & 0xFF));
    }
    out.append(reinterpret_cast<const char*>(mask), 4);
    const size_t start = out.size();
    out.append(payload.data(), payload.size());
    for (size_t i = 0; i < payload.size(); ++i)
        out[start + i] = static_cast<char>(out[start + i] ^ mask[i % 4]);
    return out;
}

// One unmasked server frame off the connection: its payload.
std::string readFrame(sys::socket_t socket, std::string& pending, http::WsOpcode& opcode)
{
    for (;;) {
        http::WsFrameHeader header;
        if (http::tryParseFrameHeader(pending, header)) {
            const size_t total = header.headerSize + static_cast<size_t>(header.payloadLen);
            readUpTo(socket, pending, total);
            expect(pending.size() >= total, "the whole frame arrives");
            expect(!header.masked, "a server frame is not masked");
            opcode = header.opcode;
            std::string payload = pending.substr(header.headerSize, static_cast<size_t>(header.payloadLen));
            pending.erase(0, total);
            return payload;
        }
        const size_t before = pending.size();
        readUpTo(socket, pending, pending.size() + 1);
        expect(pending.size() > before, "a frame header arrives");
    }
}

struct Fixture
{
    http::ServerConfig config;
    std::unique_ptr<http::Server> server;
    std::future<void> serving;

    // `sharePort` false: the port is this server's alone (where the platform lets servers share one, see ServerConfig::reusePort).
    explicit Fixture(unsigned ioThreads, bool sharePort = true)
    {
        config.host = "127.0.0.1";
        config.port = freePort();
        config.ioThreads = ioThreads;
        config.reusePort = sharePort;
        config.maxBodyBytes = 8 * 1024 * 1024;
        config.maxWebSocketMessageBytes = 4 * 1024 * 1024;
        config.maxWebSocketQueuedBytes = 16 * 1024 * 1024;
        server = std::make_unique<http::Server>(config);
    }

    void start()
    {
        std::promise<void> listening;
        serving = std::async(std::launch::async, [this, &listening] {
            server->run({}, [&listening] { listening.set_value(); });
        });
        listening.get_future().wait();
    }

    ~Fixture()
    {
        server->stop();
        if (serving.valid())
            serving.get();
    }
};

void requestsOnOneConnectionAndPipelined()
{
    Fixture f(1);
    std::atomic<int> served{0};
    f.server->onGet("/hello", [&](http::HttpContext& context) {
        ++served;
        context.text("hello " + std::to_string(served.load()));
    });
    f.start();

    const sys::socket_t socket = connectTo(f.config.port);
    std::string pending;
    for (int round = 1; round <= 50; ++round) {
        sendAll(socket, "GET /hello HTTP/1.1\r\nHost: test\r\n\r\n");
        const auto response = readResponse(socket, pending);
        expect(response.first.rfind("HTTP/1.1 200", 0) == 0, "keep-alive request is answered");
        expect(response.second == "hello " + std::to_string(round), "responses come in order");
    }
    // Forty requests in one send: every one answered, in order.
    std::string burst;
    for (int i = 0; i < 40; ++i)
        burst += "GET /hello HTTP/1.1\r\nHost: test\r\n\r\n";
    sendAll(socket, burst);
    for (int i = 1; i <= 40; ++i)
        expect(readResponse(socket, pending).second == "hello " + std::to_string(50 + i), "pipelined responses in order");
    expect(pending.empty(), "nothing beyond the responses");
    sys::closeSocket(socket);
}

void largeBodiesBothWays()
{
    Fixture f(2);
    const std::string big = pattern(3 * 1024 * 1024 + 123, 1);
    f.server->onGet("/big", [&](http::HttpContext& context) { context.text(big); });
    f.server->onGet("/pooled", [&](http::HttpContext& context) {
        http::Buffer block(f.server->bufferPool(), big.size());
        block.append(big.data(), big.size());
        context.bytes(std::move(block));
    });
    f.server->onPost("/upload", [](http::HttpContext& context) {
        // The body back, reversed in halves, so that an echo of the wrong bytes cannot pass.
        const std::string_view body = context.request().body.view();
        std::string out(body.substr(body.size() / 2));
        out.append(body.substr(0, body.size() / 2));
        context.text(out);
    });
    f.start();

    // Several connections, so that both loops serve some.
    for (int connection = 0; connection < 4; ++connection) {
        const sys::socket_t socket = connectTo(f.config.port);
        std::string pending;
        sendAll(socket, "GET /big HTTP/1.1\r\nHost: test\r\n\r\n");
        expect(readResponse(socket, pending).second == big, "a large string body arrives whole");
        sendAll(socket, "GET /pooled HTTP/1.1\r\nHost: test\r\n\r\n");
        expect(readResponse(socket, pending).second == big, "a large pooled body arrives whole");

        const std::string upload = pattern(2 * 1024 * 1024 + 77, 2 + static_cast<unsigned>(connection));
        std::string expected(upload.substr(upload.size() / 2));
        expected.append(upload.substr(0, upload.size() / 2));
        // The client writes while the server answers nothing yet, then reads while the server writes: a writer
        // thread keeps either side from waiting on a full socket.
        std::thread writer([&] {
            sendAll(socket, "POST /upload HTTP/1.1\r\nHost: test\r\nContent-Length: " + std::to_string(upload.size()) + "\r\n\r\n");
            sendAll(socket, upload);
        });
        const auto response = readResponse(socket, pending);
        writer.join();
        expect(response.second == expected, "a large request body is read whole and in order");

        // And the connection still serves a small request afterwards.
        sendAll(socket, "GET /nothing HTTP/1.1\r\nHost: test\r\n\r\n");
        expect(readResponse(socket, pending).first.rfind("HTTP/1.1 404", 0) == 0, "the connection goes on");
        sys::closeSocket(socket);
    }
}

void webSocketEchoSmallAndLarge()
{
    Fixture f(2);
    f.server->onWebSocket("/ws", [&](std::shared_ptr<http::WebSocketConnection> connection) {
        http::WebSocketConnection* raw = connection.get();
        connection->onMessage([&f, raw](std::string_view message, bool binary) {
            if (binary && message.size() >= 64 * 1024) {
                // A big one goes back as a pooled block, written in place.
                http::Buffer block(f.server->bufferPool(), message.size());
                block.append(message.data(), message.size());
                raw->send(std::move(block), true);
            } else {
                raw->send(message, binary);
            }
        });
    });
    f.start();

    for (int connection = 0; connection < 3; ++connection) {
        const sys::socket_t socket = connectTo(f.config.port);
        sendAll(socket, "GET /ws HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
        std::string pending;
        while (pending.find("\r\n\r\n") == std::string::npos) {
            const size_t before = pending.size();
            readUpTo(socket, pending, pending.size() + 1);
            expect(pending.size() > before, "the handshake is answered");
        }
        expect(pending.rfind("HTTP/1.1 101", 0) == 0, "the connection upgrades");
        pending.erase(0, pending.find("\r\n\r\n") + 4);

        http::WsOpcode opcode;
        for (int round = 0; round < 30; ++round) {
            const std::string text = "message " + std::to_string(round);
            sendAll(socket, clientFrame(http::WsOpcode::Text, text));
            expect(readFrame(socket, pending, opcode) == text && opcode == http::WsOpcode::Text, "a small frame echoes");
        }
        // Many small frames in one send, then sizes around every boundary of the paths behind send().
        std::string burst;
        for (int i = 0; i < 200; ++i)
            burst += clientFrame(http::WsOpcode::Text, "burst " + std::to_string(i));
        sendAll(socket, burst);
        for (int i = 0; i < 200; ++i)
            expect(readFrame(socket, pending, opcode) == "burst " + std::to_string(i), "a burst echoes in order");

        const size_t sizes[] = {125, 126, 4095, 8 * 1024, 8 * 1024 + 1, 65535, 65536, 300 * 1024, 2 * 1024 * 1024 + 5};
        unsigned seed = 10;
        for (const size_t size : sizes) {
            const std::string payload = pattern(size, seed++);
            std::thread writer([&] { sendAll(socket, clientFrame(http::WsOpcode::Binary, payload)); });
            const std::string echoed = readFrame(socket, pending, opcode);
            writer.join();
            expect(opcode == http::WsOpcode::Binary && echoed == payload, "a frame of every size echoes whole");
        }
        // Large and small interleaved: the order on the wire is the order they were sent in.
        const std::string large = pattern(400 * 1024, 99);
        std::thread writer([&] {
            for (int i = 0; i < 6; ++i) {
                sendAll(socket, clientFrame(http::WsOpcode::Binary, large));
                sendAll(socket, clientFrame(http::WsOpcode::Text, "after " + std::to_string(i)));
            }
        });
        for (int i = 0; i < 6; ++i) {
            expect(readFrame(socket, pending, opcode) == large, "the large frame first");
            expect(readFrame(socket, pending, opcode) == "after " + std::to_string(i), "then the small one behind it");
        }
        writer.join();

        sendAll(socket, clientFrame(http::WsOpcode::Close, std::string("\x03\xE8", 2)));
        expect(readFrame(socket, pending, opcode).size() == 2 && opcode == http::WsOpcode::Close, "the close is answered");
        sys::closeSocket(socket);
    }
}

void aTakenPortIsAnError()
{
    Fixture first(1, false);
    first.server->onGet("/", [](http::HttpContext& context) { context.text("first"); });
    first.start();

    http::ServerConfig config = first.config;  // the same port
    http::Server second(config);
    bool refused = false;
    try {
        second.run([] { return true; });
    } catch (const std::exception&) {
        refused = true;
    }
    expect(refused, "a second server on a port that is listened on fails to start");

    // And the first still serves.
    const sys::socket_t socket = connectTo(first.config.port);
    std::string pending;
    sendAll(socket, "GET / HTTP/1.1\r\nHost: test\r\n\r\n");
    expect(readResponse(socket, pending).second == "first", "the first server is untouched");
    sys::closeSocket(socket);
}

void manyConnectionsOverSeveralLoops()
{
    Fixture f(4);
    std::atomic<int> served{0};
    f.server->onGet("/n", [&](http::HttpContext& context) { context.text(std::to_string(++served)); });
    f.start();

    std::vector<std::thread> clients;
    std::atomic<int> answered{0};
    for (int client = 0; client < 8; ++client) {
        clients.emplace_back([&] {
            for (int round = 0; round < 40; ++round) {
                const sys::socket_t socket = connectTo(f.config.port);
                std::string pending;
                for (int request = 0; request < 5; ++request) {
                    sendAll(socket, "GET /n HTTP/1.1\r\nHost: test\r\n\r\n");
                    if (readResponse(socket, pending).first.rfind("HTTP/1.1 200", 0) == 0)
                        ++answered;
                }
                sys::closeSocket(socket);
            }
        });
    }
    for (auto& client : clients)
        client.join();
    expect(answered.load() == 8 * 40 * 5, "every request of every connection is answered");
    expect(served.load() == 8 * 40 * 5, "and handled exactly once");
}

}  // namespace

int main()
{
    requestsOnOneConnectionAndPipelined();
    std::printf("ok   requestsOnOneConnectionAndPipelined\n");
    largeBodiesBothWays();
    std::printf("ok   largeBodiesBothWays\n");
    webSocketEchoSmallAndLarge();
    std::printf("ok   webSocketEchoSmallAndLarge\n");
    aTakenPortIsAnError();
    std::printf("ok   aTakenPortIsAnError\n");
    manyConnectionsOverSeveralLoops();
    std::printf("ok   manyConnectionsOverSeveralLoops\n");
    return 0;
}
