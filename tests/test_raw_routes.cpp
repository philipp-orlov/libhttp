// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Server::onRaw: a route that takes a connection over as a byte stream, for proxies and tunnels.

#include "http/server.hpp"
#include "test_support.hpp"

#include <sys/socket.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using namespace testsupport;

namespace {

void expect(bool condition, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::_Exit(1);
    }
}

void sendAll(int socket, std::string_view bytes)
{
    size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t n = ::send(socket, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        expect(n > 0, "send");
        sent += static_cast<size_t>(n);
    }
}

// Reads until `size` bytes have come, or the peer closed.
std::string readUpTo(int socket, size_t size)
{
    std::string out;
    char buffer[65536];
    while (out.size() < size) {
        const ssize_t n = ::recv(socket, buffer, std::min(sizeof buffer, size - out.size()), 0);
        if (n <= 0)
            break;
        out.append(buffer, static_cast<size_t>(n));
    }
    return out;
}

std::string readToEnd(int socket)
{
    return readUpTo(socket, SIZE_MAX);
}

struct Fixture
{
    http::ServerConfig config;
    std::unique_ptr<http::Server> server;
    std::future<void> serving;
    Fixture()
    {
        config.host = "127.0.0.1";
        config.port = freePort();
        config.ioThreads = 1;
        server = std::make_unique<http::Server>(config);
        server->onGet("/hello", [](http::HttpContext& context) { context.text("hi"); });
        // proxy.test: everything echoed back as it came. big.test: a large reply, paced by onDrained.
        // eof.test: says how much it got once the client has shut down its side.
        server->onRaw([](std::string_view head) { return http::rawHeader(head, "host") == "proxy.test"; },
                      [](std::string_view, std::shared_ptr<http::RawConnection> c) {
                          http::RawConnection* raw = c.get();
                          c->onData([raw](std::string_view data) { raw->write(data); });
                      });
        server->onRaw([](std::string_view head) { return http::rawHeader(head, "Host") == "big.test"; },
                      [](std::string_view, std::shared_ptr<http::RawConnection> c) {
                          auto left = std::make_shared<size_t>(8 * 1024 * 1024);
                          http::RawConnection* raw = c.get();
                          const auto more = [raw, left] {
                              while (*left > 0 && raw->queuedBytes() < 256 * 1024) {
                                  const size_t n = std::min<size_t>(*left, 64 * 1024);
                                  raw->write(std::string(n, 'z'));
                                  *left -= n;
                              }
                              if (*left == 0)
                                  raw->close();
                          };
                          c->onData([more](std::string_view) { more(); });
                          c->onDrained(more);
                      });
        server->onRaw([](std::string_view head) { return http::rawHeader(head, "Host") == "eof.test"; },
                      [](std::string_view, std::shared_ptr<http::RawConnection> c) {
                          auto got = std::make_shared<size_t>(0);
                          http::RawConnection* raw = c.get();
                          c->onData([got](std::string_view data) { *got += data.size(); });
                          c->onEof([raw, got] {
                              raw->write("got " + std::to_string(*got));
                              raw->close();
                          });
                      });
        serving = std::async(std::launch::async, [this] { server->run(); });
        ::close(connectTo(config.port));
    }
    ~Fixture()
    {
        server->stop();
        serving.get();
    }
};

void bytesPassUnchangedAndOtherHostsAreServedAsUsual(Fixture& f)
{
    const std::string requests =
        "POST /a HTTP/1.1\r\nHost: proxy.test\r\nContent-Length: 5\r\n\r\nhello"
        "GET /b HTTP/1.1\r\nHost: proxy.test\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
    const int socket = connectTo(f.config.port);
    sendAll(socket, requests);
    expect(readUpTo(socket, requests.size()) == requests, "the claimed stream comes back byte for byte");
    ::close(socket);

    // A head that arrives a byte at a time is claimed whole.
    const int slow = connectTo(f.config.port);
    const std::string head = "GET / HTTP/1.1\r\nHost: proxy.test\r\n\r\n";
    for (const char c : head) {
        sendAll(slow, std::string_view(&c, 1));
        std::this_thread::sleep_for(1ms);
    }
    expect(readUpTo(slow, head.size()) == head, "a head sent byte by byte");
    ::close(slow);

    const int plain = connectTo(f.config.port);
    sendAll(plain, "GET /hello HTTP/1.1\r\nHost: other\r\nConnection: close\r\n\r\n");
    const std::string response = readToEnd(plain);
    expect(response.rfind("HTTP/1.1 200", 0) == 0 && response.find("\r\n\r\nhi") != std::string::npos,
           "another host gets the ordinary routes");
    ::close(plain);
}

void aKeptAliveConnectionIsClaimedAtItsNextRequest(Fixture& f)
{
    const int socket = connectTo(f.config.port);
    sendAll(socket, "GET /hello HTTP/1.1\r\nHost: other\r\n\r\n");
    std::string first;
    while (first.find("\r\n\r\nhi") == std::string::npos) {
        const std::string part = readUpTo(socket, 1);
        expect(!part.empty(), "first response");
        first += part;
    }
    const std::string second = "GET /x HTTP/1.1\r\nHost: proxy.test\r\n\r\ntail";
    sendAll(socket, second);
    expect(readUpTo(socket, second.size()) == second, "the second request is claimed");
    ::close(socket);
}

void aLargeReplyIsPacedByDrained(Fixture& f)
{
    const int socket = connectTo(f.config.port, 20);
    sendAll(socket, "GET / HTTP/1.1\r\nHost: big.test\r\n\r\n");
    std::this_thread::sleep_for(200ms);  // the client is slow at first: the handler must wait for the socket
    const auto begin = std::chrono::steady_clock::now();
    const std::string all = readToEnd(socket);
    if (all.size() != 8u * 1024 * 1024)
        std::fprintf(stderr, "got %zu bytes in %lld ms\n", all.size(),
                     static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - begin).count()));
    expect(all.size() == 8u * 1024 * 1024 && all.find_first_not_of('z') == std::string::npos,
           "8 MiB arrives whole, then the close");
    ::close(socket);
}

void aClientHalfCloseIsReportedAndRepliesStillArrive(Fixture& f)
{
    const int socket = connectTo(f.config.port);
    const std::string request = "PUT / HTTP/1.1\r\nHost: eof.test\r\n\r\n0123456789";
    sendAll(socket, request);
    ::shutdown(socket, SHUT_WR);
    expect(readToEnd(socket) == "got " + std::to_string(request.size()), "the reply after the client's end");
    ::close(socket);
}

}  // namespace

int main()
{
    Fixture f;
    bytesPassUnchangedAndOtherHostsAreServedAsUsual(f);
    std::puts("ok bytes_pass_unchanged_and_other_hosts_are_served_as_usual");
    aKeptAliveConnectionIsClaimedAtItsNextRequest(f);
    std::puts("ok a_kept_alive_connection_is_claimed_at_its_next_request");
    aLargeReplyIsPacedByDrained(f);
    std::puts("ok a_large_reply_is_paced_by_drained");
    aClientHalfCloseIsReportedAndRepliesStillArrive(f);
    std::puts("ok a_client_half_close_is_reported_and_replies_still_arrive");
    return 0;
}
