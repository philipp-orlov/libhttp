// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// ServerConfig::dropOldestWebSocketFrames: frames waiting for a slow client
// past the queue limits drop the oldest instead of closing the connection;
// ServerConfig::validateOutgoingWebSocketText = false sends the server's own
// text without the UTF-8 scan. The defaults (close; validate) stay.

#include "http/server.hpp"
#include "test_support.hpp"

#include <cstdlib>
#include <future>
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

int openSocket(uint16_t port)
{
    const int client = connectTo(port);
    sendAll(client,
            "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
            "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    char byte;
    std::string head;
    while (head.find("\r\n\r\n") == std::string::npos) {
        if (::recv(client, &byte, 1, 0) != 1)
            std::_Exit(90);

        head.push_back(byte);
    }
    return client;
}

void sendText(int client, std::string_view text)
{
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    frame.push_back(static_cast<char>(0x80 | text.size()));
    frame.append(4, '\0');  // a zero mask leaves the payload as it is
    frame.append(text.data(), text.size());
    sendAll(client, frame);
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

// The next frame's opcode and payload; false once the connection is gone.
bool readFrame(int client, int& opcode, std::string& payload)
{
    unsigned char header[2];
    if (!receiveExactly(client, reinterpret_cast<char*>(header), 2))
        return false;

    opcode = header[0] & 0x0F;
    size_t length = header[1] & 0x7F;
    if (length == 126) {
        unsigned char extended[2];
        if (!receiveExactly(client, reinterpret_cast<char*>(extended), 2))
            return false;

        length = static_cast<size_t>(extended[0]) << 8 | extended[1];
    }
    payload.resize(length);
    return receiveExactly(client, &payload[0], length);
}

void runServer(bool dropOldest)
{
    http::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = freePort();
    config.ioThreads = 1;
    config.workerThreads = 1;
    config.maxWebSocketQueuedFrames = 4;
    if (!dropOldest) {
        // A tiny task queue, filled by the handler below: the close must not release the running
        // handler's callbacks inline just because the loop's queue has no room.
        config.reactorTaskCapacity = 8;
        config.workerQueueCapacity = 2;
    }
    config.dropOldestWebSocketFrames = dropOldest;
    config.validateOutgoingWebSocketText = !dropOldest;
    http::Server server(config);
    // Where the connection stays open its handle is kept here. Where a send closes it (the default
    // run) it is not: the server must itself keep the handler alive until the handler returns.
    static std::vector<std::shared_ptr<http::WebSocketConnection>> sockets;
    server.onWebSocket("/ws", [dropOldest](std::shared_ptr<http::WebSocketConnection> connection) {
        if (dropOldest)
            sockets.push_back(connection);
        http::WebSocketConnection* raw = connection.get();
        connection->onMessage([raw, dropOldest](std::string_view data, bool) {
            if (data == "flood") {
                if (!dropOldest)
                    while (raw->post([] {})) {
                    }
                // Corked while the message is handled: more than the queue takes.
                for (int index = 0; index < 200; ++index) {
                    std::string frame = "frame " + std::to_string(index) + " ";
                    frame.resize(4096, '.');
                    raw->send(frame);
                }
            } else if (data == "raw") {
                raw->send(std::string_view("\xff\xfe", 2));
            } else {
                raw->send(data);
            }
        });
    });
    auto serving = std::async(std::launch::async, [&] { server.run(); });

    const int client = openSocket(config.port);
    sendText(client, "flood");
    int opcode = 0;
    std::string payload;
    if (dropOldest) {
        // The newest four made it, in order; the connection is still open.
        for (int index = 196; index < 200; ++index) {
            expect(readFrame(client, opcode, payload) && opcode == 1, 1, "frame kept");
            expect(payload.rfind("frame " + std::to_string(index) + " ", 0) == 0, 2,
                   "the oldest frames are the ones dropped");
        }
        sendText(client, "raw");
        expect(readFrame(client, opcode, payload) && opcode == 1 && payload == "\xff\xfe", 3,
               "own text sent unvalidated");
        sendText(client, "still open");
        expect(readFrame(client, opcode, payload) && payload == "still open", 4,
               "echo after drops");
    } else {
        // The default: a client this slow is cut off.
        bool closed = false;
        for (int frames = 0; frames < 300 && !closed; ++frames)
            closed = !readFrame(client, opcode, payload) || opcode == 8;
        expect(closed, 5, "connection closed at the queue limit");
        const int other = openSocket(config.port);
        sendText(other, "raw");
        expect(!readFrame(other, opcode, payload) || opcode == 8, 6,
               "invalid UTF-8 text refused by default");
        ::close(other);
    }
    ::close(client);
    server.stop();
    serving.get();
    sockets.clear();
}
}  // namespace

int main()
{
    runServer(true);
    runServer(false);
    std::puts("websocket back-pressure: drop-oldest, unvalidated own text and the defaults passed");
}
