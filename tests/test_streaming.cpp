// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Server::onStream() end to end: the head goes out with no Content-Length
// and `Connection: close`, frames pushed from another thread (via post())
// arrive as raw bytes with no WebSocket framing, and the client closing its
// socket fires onClose().

#include "http/server.hpp"
#include "http/streaming_connection.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>
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

    std::promise<std::shared_ptr<http::StreamingConnection>> attached;
    std::atomic<bool> closed{false};
    server.onStream("/mjpeg", [&](http::HttpContext& context, std::shared_ptr<http::StreamingConnection> connection) {
        context.header("Content-Type", "multipart/x-mixed-replace; boundary=frame");
        connection->onClose([&closed] { closed = true; });
        attached.set_value(connection);
    });

    auto serving = std::async(std::launch::async, [&] { server.run(); });
    int client = connectTo(config.port);
    const std::string request = "GET /mjpeg HTTP/1.1\r\nHost: x\r\n\r\n";
    ::send(client, request.data(), request.size(), MSG_NOSIGNAL);

    auto connection = attached.get_future().get();

    std::string received;
    char bytes[4096];
    while (received.find("\r\n\r\n") == std::string::npos) {
        const ssize_t count = ::recv(client, bytes, sizeof(bytes), 0);
        if (count <= 0)
            throw std::runtime_error("missing stream head");

        received.append(bytes, static_cast<size_t>(count));
    }
    const std::string head = received.substr(0, received.find("\r\n\r\n"));
    if (head.find("HTTP/1.1 200") != 0)
        return 2;
    if (head.find("multipart/x-mixed-replace; boundary=frame") == std::string::npos)
        return 3;
    if (head.find("Connection: close") == std::string::npos)
        return 4;
    if (head.find("Content-Length") != std::string::npos)
        return 5;

    // Pushed from a thread that is not the connection's own loop, exactly
    // how a worker thread encoding a frame would call it: post() first,
    // write() only once back on the loop.
    std::promise<void> pushed;
    connection->post([connection, &pushed] {
        const std::string frame = "--frame\r\nContent-Type: image/jpeg\r\n\r\nJPEGBYTES\r\n";
        if (!connection->write(frame.data(), frame.size()))
            throw std::runtime_error("first write must not be dropped");

        pushed.set_value();
    });
    pushed.get_future().get();

    received.clear();
    while (received.find("JPEGBYTES") == std::string::npos) {
        const ssize_t count = ::recv(client, bytes, sizeof(bytes), 0);
        if (count <= 0)
            throw std::runtime_error("missing pushed frame");

        received.append(bytes, static_cast<size_t>(count));
    }
    if (received.find("--frame\r\nContent-Type: image/jpeg\r\n\r\nJPEGBYTES\r\n") != 0)
        return 6;

    ::close(client);
    for (int waited = 0; waited < 500 && !closed; ++waited)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!closed)
        return 7;

    server.stop();
    serving.get();
    std::puts("streaming: head framing, pushed frames and client-close detection passed");
    return 0;
}
