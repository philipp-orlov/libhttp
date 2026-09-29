// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/server.hpp"
#include "http/http_context.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <future>
#include <cstdio>
#include <stdexcept>

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
    config.reactorTaskCapacity = 1;
    config.buffers.initialSlabs[0] = 1;
    config.buffers.initialSlabs[2] = 1;
    config.buffers.sealed = true;
    ::close(reservation);
    http::Server server(config);
    std::promise<void> entered;
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::weak_ptr<http::WebSocketConnection> websocket;
    http::IoLoop* requestLoop = nullptr;
    server.onWebSocket("/ws", [&](auto connection) {
        websocket = connection;
        connection->send("ready");
    });
    server.onGet("/pool", [](http::HttpContext& context) { context.text("pool-owner"); });
    server.onPost("/echo", [](http::HttpContext& context, std::function<void()> done) {
        context.text("echo:" + std::to_string(context.request().body.size()));
        done();
    });
    server.onPost("/work", [&](http::HttpContext& context, std::function<void()> done) {
        requestLoop = &context.loop();
        server.pool().enqueue([&, done] {
            entered.set_value();
            gate.wait();
            context.loop().post([&, done] {
                context.text("finished");
                done();
                done();
            });
        });
    });
    auto serving = std::async(std::launch::async, [&] { server.run(); });
    int client = connectTo(config.port);
    std::string handshake =
        "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: "
        "websocket\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: "
        "dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    ::send(client, handshake.data(), handshake.size(), MSG_NOSIGNAL);
    std::string received;
    char bytes[4096];
    while (received.find("ready") == std::string::npos) {
        ssize_t count = ::recv(client, bytes, sizeof(bytes), 0);
        if (count <= 0)
            throw std::runtime_error("missing WebSocket handshake/frame");

        received.append(bytes, static_cast<size_t>(count));
    }
    if (received.find("HTTP/1.1 101") != 0 || received.find("\r\n\r\n") > received.find("ready"))
        return 2;

    ::close(client);
    client = connectTo(config.port);
    const std::string firstRequest = "GET /pool HTTP/1.1\r\nHost: x\r\n\r\n";
    ::send(client, firstRequest.data(), firstRequest.size(), MSG_NOSIGNAL);
    received.clear();
    while (received.find("pool-owner") == std::string::npos) {
        const ssize_t count = ::recv(client, bytes, sizeof(bytes), 0);
        if (count <= 0)
            throw std::runtime_error("missing keep-alive response");

        received.append(bytes, static_cast<size_t>(count));
    }
    // A client that sends Expect: 100-continue holds the body back until the
    // interim response arrives (libcurl for a full second otherwise).
    const std::string expecting =
        "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n";
    ::send(client, expecting.data(), expecting.size(), MSG_NOSIGNAL);
    received.clear();
    while (received.find("\r\n\r\n") == std::string::npos) {
        const ssize_t count = ::recv(client, bytes, sizeof(bytes), 0);
        if (count <= 0)
            throw std::runtime_error("missing 100 Continue");

        received.append(bytes, static_cast<size_t>(count));
    }
    if (received.find("HTTP/1.1 100 Continue\r\n\r\n") != 0)
        return 7;

    ::send(client, "hello", 5, MSG_NOSIGNAL);
    received.clear();
    while (received.find("echo:5") == std::string::npos) {
        const ssize_t count = ::recv(client, bytes, sizeof(bytes), 0);
        if (count <= 0)
            throw std::runtime_error("missing response after 100 Continue");

        received.append(bytes, static_cast<size_t>(count));
    }
    if (received.find("HTTP/1.1 200") != 0)
        return 8;

    std::string request = "POST /work HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello";
    ::send(client, request.data(), request.size(), MSG_NOSIGNAL);
    if (entered.get_future().wait_for(std::chrono::seconds(5)) != std::future_status::ready)
        throw std::runtime_error("worker did not start");

    std::promise<void> reactorEntered;
    std::promise<void> reactorRelease;
    std::promise<void> queueDrained;
    auto reactorGate = reactorRelease.get_future().share();
    requestLoop->post([&] {
        reactorEntered.set_value();
        reactorGate.wait();
    });
    reactorEntered.get_future().get();
    requestLoop->post([&] { queueDrained.set_value(); });
    if (requestLoop->tryPost([] {}))
        throw std::runtime_error("reactor queue did not enforce capacity");

    server.stop();
    bool drainedTooSoon =
        serving.wait_for(std::chrono::milliseconds(20)) == std::future_status::ready;
    reactorRelease.set_value();
    queueDrained.get_future().get();
    release.set_value();
    serving.get();
    ::close(client);
    if (drainedTooSoon || !websocket.expired())
        return 3;

    if (server.bufferStats().outstandingBlocks != 0 || !server.bufferStats().allocations)
        return 4;

    if (http::BufferPool::shared().stats().allocations != 0)
        return 5;

    http::Server throwing(config);
    bool controlFailure = false;
    try {
        throwing.run([]() -> bool { throw std::runtime_error("stop predicate failed"); });
    } catch (const std::runtime_error& error) {
        controlFailure = std::string_view(error.what()) == "stop predicate failed";
    }
    if (!controlFailure || throwing.bufferStats().outstandingBlocks)
        return 6;

    std::puts(
        "server: handshake ordering, 100 Continue, worker shutdown drain, WebSocket and buffer "
        "retirement passed");
}