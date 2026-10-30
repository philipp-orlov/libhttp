// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// An accepted descriptor is closed exactly once when setting the connection
// up fails after its TcpConnection exists (here: the per-address count's map
// node cannot be allocated, with maxConnectionsPerIp set). A second close
// could hit a descriptor number another thread has just been given.

#include "http/http_context.hpp"
#include "http/server.hpp"
#include "test_support.hpp"

#include <sys/syscall.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <future>
#include <new>

using namespace testsupport;

namespace {
thread_local bool t_test = false;
std::atomic<int> g_failIn{0};  // fail the n-th server-side allocation from now, 0 = never
std::atomic<int> g_failed{0};
std::atomic<int> g_badCloses{0};

void* allocate(size_t size)
{
    if (!t_test && g_failIn.load() > 0 && g_failIn.fetch_sub(1) == 1) {
        ++g_failed;
        throw std::bad_alloc();
    }
    if (void* pointer = std::malloc(size ? size : 1))
        return pointer;

    throw std::bad_alloc();
}
}  // namespace

void* operator new(size_t size)
{
    return allocate(size);
}
void operator delete(void* pointer) noexcept
{
    std::free(pointer);
}
void operator delete(void* pointer, size_t) noexcept
{
    std::free(pointer);
}

// Every close() of the program comes here; one that finds no such descriptor
// on a server thread is the double close.
extern "C" int close(int fd)
{
    const long result = ::syscall(SYS_close, fd);
    if (result != 0 && errno == EBADF && !t_test)
        ++g_badCloses;

    return static_cast<int>(result);
}

int main()
{
    t_test = true;
    http::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = freePort();
    config.ioThreads = 1;
    config.workerThreads = 1;
    config.maxConnectionsPerIp = 4;
    http::Server server(config);
    server.onGet("/ping", [](http::HttpContext& context) { context.text("pong"); });
    auto serving = std::async(std::launch::async, [&] { server.run(); });

    // A first connection creates the Session the next one reuses, so that
    // the allocations of an accept are only those of its connection.
    if (roundTrip(config.port, "GET /ping HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            .find("pong") == std::string::npos)
        std::_Exit(1);

    // The TcpConnection and its state, then the map node for the address.
    g_failIn = 3;
    const int refused = connectTo(config.port);
    char byte;
    const ssize_t got = ::recv(refused, &byte, 1, 0);
    ::close(refused);
    if (got != 0 || g_failed != 1) {
        std::fprintf(stderr, "recv=%zd failed allocations=%d\n", got, g_failed.load());
        std::_Exit(2);
    }
    if (g_badCloses != 0) {
        std::fprintf(stderr, "a descriptor was closed twice\n");
        std::_Exit(3);
    }
    if (roundTrip(config.port, "GET /ping HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
            .find("pong") == std::string::npos)
        std::_Exit(4);

    server.stop();
    serving.get();
    std::puts(
        "accept failure: the descriptor of a connection that failed to set up is closed once "
        "passed");
}
