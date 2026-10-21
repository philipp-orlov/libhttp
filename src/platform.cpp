// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/platform.hpp"

#include <cstring>
#ifndef _WIN32
#include <netdb.h>
#endif

namespace http {

bool resolveListenAddress(const std::string& hostText, uint16_t port, ListenAddress& out)
{
    std::string host = hostText;
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);

    out = ListenAddress{};
    if (host.empty() || host == "0.0.0.0" || host == "*") {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        std::memcpy(&out.storage, &address, sizeof(address));
        out.length = sizeof(address);
        return true;
    }
    if (host == "::") {
        sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        address.sin6_port = htons(port);
        address.sin6_addr = in6addr_any;
        std::memcpy(&out.storage, &address, sizeof(address));
        out.length = sizeof(address);
        out.family = AF_INET6;
        out.dualStack = true;
        return true;
    }
    sockaddr_in v4{};
    if (inet_pton(AF_INET, host.c_str(), &v4.sin_addr) == 1) {
        v4.sin_family = AF_INET;
        v4.sin_port = htons(port);
        std::memcpy(&out.storage, &v4, sizeof(v4));
        out.length = sizeof(v4);
        return true;
    }
    sockaddr_in6 v6{};
    if (inet_pton(AF_INET6, host.c_str(), &v6.sin6_addr) == 1) {
        v6.sin6_family = AF_INET6;
        v6.sin6_port = htons(port);
        std::memcpy(&out.storage, &v6, sizeof(v6));
        out.length = sizeof(v6);
        out.family = AF_INET6;
        return true;
    }
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0 || !found)
        return false;

    const addrinfo* chosen = found;
    for (const addrinfo* entry = found; entry; entry = entry->ai_next) {
        if (entry->ai_family == AF_INET) {
            chosen = entry;
            break;
        }
    }
    bool usable = false;
    if (chosen->ai_addrlen <= sizeof(out.storage) &&
        (chosen->ai_family == AF_INET || chosen->ai_family == AF_INET6)) {
        std::memcpy(&out.storage, chosen->ai_addr, chosen->ai_addrlen);
        out.length = static_cast<socklen_t>(chosen->ai_addrlen);
        out.family = chosen->ai_family;
        if (out.family == AF_INET)
            reinterpret_cast<sockaddr_in*>(&out.storage)->sin_port = htons(port);
        else
            reinterpret_cast<sockaddr_in6*>(&out.storage)->sin6_port = htons(port);

        usable = true;
    }
    freeaddrinfo(found);
    return usable;
}

#if defined(HTTP_WINDOWS)

Pal::Pal()
{
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
}

Pal::~Pal()
{
    WSACleanup();
}

void closeSocket(socket_t s)
{
    if (s != kInvalidSocket)
        closesocket(s);
}

bool setNonBlocking(socket_t s)
{
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}

bool setTcpNoDelay(socket_t s)
{
    int flag = 1;
    return setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag),
                      sizeof(flag)) == 0;
}

bool setReuseAddr(socket_t s)
{
    int flag = 1;
    return setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&flag),
                      sizeof(flag)) == 0;
}

bool setReusePort(socket_t)
{
    // Windows has no direct SO_REUSEPORT equivalent; SO_REUSEADDR combined
    // with a single accept-thread dispatch model is used instead.
    return true;
}

int lastSocketError()
{
    return WSAGetLastError();
}

bool wouldBlock(int err)
{
    return err == WSAEWOULDBLOCK;
}

#else  // Linux / POSIX

Pal::Pal() = default;
Pal::~Pal() = default;

void closeSocket(socket_t s)
{
    if (s != kInvalidSocket)
        ::close(s);
}

bool setNonBlocking(socket_t s)
{
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0)
        return false;

    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool setTcpNoDelay(socket_t s)
{
    int flag = 1;
    return setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) == 0;
}

bool setReuseAddr(socket_t s)
{
    int flag = 1;
    return setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) == 0;
}

bool setReusePort(socket_t s)
{
#ifdef SO_REUSEPORT
    int flag = 1;
    return setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &flag, sizeof(flag)) == 0;
#else
    (void)s;
    return false;
#endif
}

int lastSocketError()
{
    return errno;
}

bool wouldBlock(int err)
{
    return err == EAGAIN || err == EWOULDBLOCK;
}

#endif

}  // namespace http
