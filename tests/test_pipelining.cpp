// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The corked output path end to end: pipelined requests in one segment are
// answered in order; a response body too large to cork still arrives whole
// and the connection stays usable; a WebSocket upgrade pipelined behind an
// ordinary request, with a frame sent on the heels of the handshake, is
// answered after that request; and several frames in one segment come back
// in order.

#include "http/server.hpp"
#include "http/websocket.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
int connectTo(uint16_t port)
{
    for (int attempt = 0; attempt < 1000; ++attempt) {
        int socket = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            timeval timeout{5, 0};
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            return socket;
        }
        ::close(socket);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("server did not start");
}

void sendAll(int socket, std::string_view data)
{
    while (!data.empty()) {
        const ssize_t n = ::send(socket, data.data(), data.size(), 0);
        if (n <= 0)
            throw std::runtime_error("send failed");

        data.remove_prefix(static_cast<size_t>(n));
    }
}

// Reads until `buffer` holds at least `bytes` bytes (or the peer closes).
bool readAtLeast(int socket, std::string& buffer, size_t bytes)
{
    char chunk[65536];
    while (buffer.size() < bytes) {
        const ssize_t n = ::recv(socket, chunk, sizeof(chunk), 0);
        if (n <= 0)
            return false;

        buffer.append(chunk, static_cast<size_t>(n));
    }
    return true;
}

// Consumes one HTTP response from the front of `buffer`, returning its body.
bool takeResponse(int socket, std::string& buffer, std::string& body, std::string& head)
{
    size_t headEnd;
    while ((headEnd = buffer.find("\r\n\r\n")) == std::string::npos) {
        if (!readAtLeast(socket, buffer, buffer.size() + 1))
            return false;
    }
    head = buffer.substr(0, headEnd + 4);
    const size_t lengthAt = head.find("Content-Length: ");
    size_t length = 0;
    if (lengthAt != std::string::npos)
        length = std::stoul(head.substr(lengthAt + 16));

    if (!readAtLeast(socket, buffer, headEnd + 4 + length))
        return false;

    body = buffer.substr(headEnd + 4, length);
    buffer.erase(0, headEnd + 4 + length);
    return true;
}

std::string maskedFrame(std::string_view payload, uint8_t opcode = 0x1)
{
    std::string frame;
    frame.push_back(static_cast<char>(0x80 | opcode));
    if (payload.size() <= 125) {
        frame.push_back(static_cast<char>(0x80 | payload.size()));
    } else {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>(payload.size() >> 8));
        frame.push_back(static_cast<char>(payload.size() & 0xFF));
    }
    const uint8_t key[4] = {0x12, 0x34, 0x56, 0x78};
    frame.append(reinterpret_cast<const char*>(key), 4);
    for (size_t i = 0; i < payload.size(); ++i)
        frame.push_back(static_cast<char>(static_cast<uint8_t>(payload[i]) ^ key[i % 4]));

    return frame;
}

// One unmasked server frame from the front of `buffer`: returns its payload.
bool takeFrame(int socket, std::string& buffer, std::string& payload)
{
    if (!readAtLeast(socket, buffer, 2))
        return false;

    size_t length = static_cast<uint8_t>(buffer[1]) & 0x7F;
    size_t headerSize = 2;
    if (length == 126) {
        if (!readAtLeast(socket, buffer, 4))
            return false;

        length = (static_cast<uint8_t>(buffer[2]) << 8) | static_cast<uint8_t>(buffer[3]);
        headerSize = 4;
    }
    if (!readAtLeast(socket, buffer, headerSize + length))
        return false;

    payload = buffer.substr(headerSize, length);
    buffer.erase(0, headerSize + length);
    return true;
}
}  // namespace

int main()
{
    int reservation = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(reservation, reinterpret_cast<sockaddr*>(&address), sizeof(address)))
        return 1;

    socklen_t addressSize = sizeof(address);
    getsockname(reservation, reinterpret_cast<sockaddr*>(&address), &addressSize);
    http::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = ntohs(address.sin_port);
    config.ioThreads = 1;
    config.workerThreads = 1;
    ::close(reservation);
    http::Server server(config);

    const std::string large(300 * 1024, 'L');  // past the corking limit
    server.onGet("/n/{n}", [](http::HttpContext& context) {
        context.text(std::string("n=") + std::string(context.routeParam("n")));
    });
    server.onGet("/large", [&](http::HttpContext& context) { context.text(large); });
    server.onGet("/later", [&](http::HttpContext& context, std::function<void()> done) {
        // Answered from a worker: the requests pipelined behind it must wait
        // for it and still come back in order.
        server.pool().enqueue([&context, done] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            context.loop().post([&context, done] {
                context.text("later");
                done();
            });
        });
    });
    server.onWebSocket("/ws", [](std::shared_ptr<http::WebSocketConnection> connection) {
        connection->onMessage([connection](std::string_view data, bool binary) {
            connection->send(std::string("echo:") + std::string(data), binary);
        });
    });

    std::promise<void> release;
    auto serving = std::async(std::launch::async, [&] {
        server.run([&, gate = release.get_future().share()] {
            return gate.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        });
    });

    int result = 0;
    try {
        // 1. Five pipelined requests in one segment, one of them answered
        //    later, one with a body too large to cork.
        int client = connectTo(config.port);
        sendAll(client,
                "GET /n/1 HTTP/1.1\r\nHost: x\r\n\r\n"
                "GET /later HTTP/1.1\r\nHost: x\r\n\r\n"
                "GET /n/2 HTTP/1.1\r\nHost: x\r\n\r\n"
                "GET /large HTTP/1.1\r\nHost: x\r\n\r\n"
                "GET /n/3 HTTP/1.1\r\nHost: x\r\n\r\n");
        std::string buffer, body, head;
        const char* expected[] = {"n=1", "later", "n=2", nullptr, "n=3"};
        for (int i = 0; i < 5; ++i) {
            if (!takeResponse(client, buffer, body, head) || head.rfind("HTTP/1.1 200", 0) != 0) {
                std::fprintf(stderr, "response %d missing or not 200\n", i);
                return 2;
            }
            const std::string want = expected[i] ? expected[i] : large;
            if (body != want) {
                std::fprintf(stderr, "response %d: wrong body (%zu bytes)\n", i, body.size());
                return 3;
            }
        }
        // The connection is still good for more.
        sendAll(client, "GET /n/4 HTTP/1.1\r\nHost: x\r\n\r\n");
        if (!takeResponse(client, buffer, body, head) || body != "n=4")
            return 4;

        // 2. A request, then an upgrade, then a frame -- all in one segment.
        sendAll(client,
                "GET /n/5 HTTP/1.1\r\nHost: x\r\n\r\n"
                "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n" +
                    maskedFrame("first"));
        if (!takeResponse(client, buffer, body, head) || body != "n=5")
            return 5;

        if (!takeResponse(client, buffer, body, head) || head.rfind("HTTP/1.1 101", 0) != 0 ||
            head.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == std::string::npos)
            return 6;

        std::string payload;
        if (!takeFrame(client, buffer, payload) || payload != "echo:first")
            return 7;

        // 3. Three frames in one segment, one of them larger than 125 bytes.
        const std::string big(3000, 'b');
        sendAll(client, maskedFrame("a") + maskedFrame(big) + maskedFrame("c"));
        const std::string wants[] = {"echo:a", "echo:" + big, "echo:c"};
        for (const auto& want : wants) {
            if (!takeFrame(client, buffer, payload) || payload != want)
                return 8;
        }
        // A frame split across two segments, the split inside the payload.
        const std::string frame = maskedFrame(std::string(20000, 's'));
        sendAll(client, std::string_view(frame).substr(0, 7000));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        sendAll(client, std::string_view(frame).substr(7000));
        if (!takeFrame(client, buffer, payload) || payload != "echo:" + std::string(20000, 's'))
            return 9;

        // Close handshake.
        sendAll(client, maskedFrame(std::string("\x03\xe8", 2), 0x8));
        if (!takeFrame(client, buffer, payload) || payload != std::string("\x03\xe8", 2))
            return 10;

        ::close(client);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        result = 11;
    }

    release.set_value();
    serving.get();
    if (result == 0)
        std::puts("pipelining: ordered corked responses, large body, pipelined upgrade, frame bursts passed");

    return result;
}
