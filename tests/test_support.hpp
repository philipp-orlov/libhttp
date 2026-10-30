// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

// Helpers shared by the socket-level server tests.
namespace testsupport {

// A free loopback port (bound and released; racy but fine for a test).
inline uint16_t freePort()
{
    int reservation = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(reservation, reinterpret_cast<sockaddr*>(&address), sizeof(address)))
        throw std::runtime_error("bind");

    socklen_t size = sizeof(address);
    getsockname(reservation, reinterpret_cast<sockaddr*>(&address), &size);
    ::close(reservation);
    return ntohs(address.sin_port);
}

inline int connectTo(uint16_t port, int receiveTimeoutSeconds = 5)
{
    for (int attempt = 0; attempt < 2000; ++attempt) {
        int socket = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            timeval timeout{receiveTimeoutSeconds, 0};
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            return socket;
        }
        ::close(socket);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("server did not start");
}

inline void sendAll(int socket, const std::string& data)
{
    ::send(socket, data.data(), data.size(), MSG_NOSIGNAL);
}

// Reads until `marker` appears, the peer closes, or the receive timeout hits.
inline std::string readUntil(int socket, const std::string& marker)
{
    std::string received;
    char bytes[4096];
    while (received.find(marker) == std::string::npos) {
        const ssize_t count = ::recv(socket, bytes, sizeof(bytes), 0);
        if (count <= 0)
            break;

        received.append(bytes, static_cast<size_t>(count));
    }
    return received;
}

// Reads until the peer closes or the receive timeout hits.
inline std::string readAll(int socket)
{
    return readUntil(socket, std::string(1, '\0'));
}

// One request on a fresh connection; the whole response (server closes).
inline std::string roundTrip(uint16_t port, const std::string& request)
{
    const int socket = connectTo(port);
    sendAll(socket, request);
    std::string response = readAll(socket);
    ::close(socket);
    return response;
}

}  // namespace testsupport
