// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// EXT-11: listen on IPv6, dual-stack and named hosts.

#include "http/http_context.hpp"
#include "http/server.hpp"
#include "test_support.hpp"

#include <netdb.h>

#include <cstdlib>
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

bool ipv6Available()
{
    const int probe = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (probe < 0)
        return false;

    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_loopback;
    const bool bound = ::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    ::close(probe);
    return bound;
}

std::string getOver(int family, uint16_t port)
{
    for (int attempt = 0; attempt < 2000; ++attempt) {
        const int socket = ::socket(family, SOCK_STREAM, 0);
        int result = -1;
        if (family == AF_INET6) {
            sockaddr_in6 address{};
            address.sin6_family = AF_INET6;
            address.sin6_addr = in6addr_loopback;
            address.sin6_port = htons(port);
            result = ::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        } else {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port);
            result = ::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        }
        if (result == 0) {
            timeval timeout{3, 0};
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            sendAll(socket, "GET /ping HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
            std::string reply = readAll(socket);
            ::close(socket);
            return reply;
        }
        ::close(socket);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return {};
}

// Serves on `host`; returns the replies to an IPv4 and an IPv6 request ("" = refused).
std::pair<std::string, std::string> serveOn(const std::string& host)
{
    http::ServerConfig config;
    config.host = host;
    config.port = freePort();
    config.ioThreads = 1;
    config.workerThreads = 1;
    http::Server server(config);
    server.onGet("/ping", [](http::HttpContext& context) { context.text("pong"); });
    auto serving = std::async(std::launch::async, [&] { server.run(); });
    std::pair<std::string, std::string> replies;
    // Give the listener time to come up before probing the family that must fail.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    replies.first = getOver(AF_INET, config.port);
    replies.second = getOver(AF_INET6, config.port);
    server.stop();
    serving.get();
    return replies;
}
}  // namespace

int main()
{
    // Names and IPv4 literals keep working.
    expect(serveOn("127.0.0.1").first.find("pong") != std::string::npos, 1, "IPv4 literal");
    expect(serveOn("localhost").first.find("pong") != std::string::npos, 2, "hostname");

    // An address that is not one is an error, not a silent all-interfaces bind.
    {
        http::ServerConfig config;
        config.host = "not a host!";
        config.port = freePort();
        config.ioThreads = 1;
        config.workerThreads = 1;
        http::Server server(config);
        bool thrown = false;
        try {
            server.run();
        } catch (const std::invalid_argument&) {
            thrown = true;
        }
        expect(thrown, 3, "invalid listen address refused");
    }
    if (!ipv6Available()) {
        std::puts("listen addresses: IPv4 and names passed (IPv6 not available here, skipped)");
        return 0;
    }
    auto loopback = serveOn("::1");
    expect(loopback.second.find("pong") != std::string::npos, 4, "IPv6 loopback served");
    expect(loopback.first.empty(), 5, "a specific IPv6 address does not take IPv4");
    auto bracketed = serveOn("[::1]");
    expect(bracketed.second.find("pong") != std::string::npos, 6, "bracketed IPv6 literal");
    auto dual = serveOn("::");
    expect(dual.first.find("pong") != std::string::npos &&
               dual.second.find("pong") != std::string::npos,
           7, "\"::\" serves IPv4 and IPv6");
    std::puts("listen addresses: IPv4, names, IPv6 literal and dual-stack passed");
}
