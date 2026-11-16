// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// A memory soak of the server over real sockets, on every platform: clients keep connecting, asking for bodies of
// every size, uploading, pipelining, upgrading to WebSocket and echoing frames small and large -- and now and then
// drop the connection in the middle of something. The process's memory is sampled as it goes; after a warm-up it must
// stay level. Not part of the test run by default (it takes as long as it is told to):
//
//   http_soak_portable [seconds=60] [clients=6]
//
// Exits 1 when the memory in use at the end is more than a quarter above what it was after the warm-up.
//
// Connections are long-lived on purpose (hundreds to thousands of exchanges each). A test that opens thousands of
// short connections from one host to one port measures the TCP stack instead: Windows drops the SYN of a connection
// whose source port was used by one the server closed in the last two minutes (its end is in TIME_WAIT), and the
// clients then sit in connect() for seconds at a time. The line printed under each sample says where the clients are
// (1 = connecting) and why connects failed, so that such a stall is not mistaken for the server.

#include "http/server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "http/websocket.hpp"
#include "sys/net.hpp"

#ifdef _WIN32
#include <psapi.h>
#else
#include <fstream>
#endif

namespace {

const sys::Pal networking;

// Bytes the process has committed for itself (private bytes on Windows, resident set elsewhere).
size_t memoryInUse()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof counters;
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof counters);
    return counters.PrivateUsage;
#else
    std::ifstream statm("/proc/self/statm");
    size_t total = 0, resident = 0;
    statm >> total >> resident;
    return resident * 4096;
#endif
}

uint16_t freePort()
{
    const sys::socket_t reservation = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ::bind(reservation, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    socklen_t size = sizeof(address);
    getsockname(reservation, reinterpret_cast<sockaddr*>(&address), &size);
    sys::closeSocket(reservation);
    return ntohs(address.sin_port);
}

// Why connects failed, by error code (the last few distinct ones), for the line printed with each sample.
std::atomic<uint64_t> g_connectFailures{0};
std::atomic<int> g_lastConnectError{0};

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
            const DWORD timeout = 20000;
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
#else
            timeval timeout{20, 0};
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
            return socket;
        }
        g_lastConnectError.store(sys::lastSocketError(), std::memory_order_relaxed);
        ++g_connectFailures;
        sys::closeSocket(socket);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return sys::kInvalidSocket;
}

bool sendAll(sys::socket_t socket, std::string_view bytes)
{
    size_t sent = 0;
    while (sent < bytes.size()) {
#ifdef _WIN32
        const int n = ::send(socket, bytes.data() + sent, static_cast<int>(std::min<size_t>(bytes.size() - sent, 1 << 20)), 0);
#else
        const auto n = ::send(socket, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
#endif
        if (n <= 0)
            return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool readUpTo(sys::socket_t socket, std::string& out, size_t size)
{
    char buffer[65536];
    while (out.size() < size) {
        const auto n = ::recv(socket, buffer, static_cast<int>(std::min(sizeof buffer, size - out.size())), 0);
        if (n <= 0)
            return false;
        out.append(buffer, static_cast<size_t>(n));
    }
    return true;
}

// One response with a Content-Length: true when its body is `expected` bytes long.
bool readResponse(sys::socket_t socket, std::string& pending, size_t expected)
{
    size_t end;
    while ((end = pending.find("\r\n\r\n")) == std::string::npos)
        if (!readUpTo(socket, pending, pending.size() + 1))
            return false;
    const size_t at = pending.find("Content-Length: ");
    if (at == std::string::npos || at > end)
        return false;
    const size_t length = static_cast<size_t>(std::strtoull(pending.c_str() + at + 16, nullptr, 10));
    if (!readUpTo(socket, pending, end + 4 + length))
        return false;
    pending.erase(0, end + 4 + length);
    return length == expected;
}

std::string clientFrame(http::WsOpcode opcode, std::string_view payload)
{
    std::string out;
    out.push_back(static_cast<char>(0x80 | static_cast<int>(opcode)));
    const uint8_t mask[4] = {0x5a, 0x21, 0x7e, 0x13};
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

bool readFrame(sys::socket_t socket, std::string& pending, size_t& payloadSize)
{
    for (;;) {
        http::WsFrameHeader header;
        if (http::tryParseFrameHeader(pending, header)) {
            const size_t total = header.headerSize + static_cast<size_t>(header.payloadLen);
            if (!readUpTo(socket, pending, total))
                return false;
            payloadSize = static_cast<size_t>(header.payloadLen);
            pending.erase(0, total);
            return true;
        }
        if (!readUpTo(socket, pending, pending.size() + 1))
            return false;
    }
}

// What each client is doing, for the line printed when nothing moves.
std::atomic<int> g_state[64];
thread_local int t_client = 0;
void at(int state) { g_state[t_client].store(state, std::memory_order_relaxed); }

struct Counters
{
    std::atomic<uint64_t> requests{0}, frames{0}, connections{0}, dropped{0}, errors{0};
};

// A size from a distribution with a long tail: mostly small, sometimes hundreds of KiB.
size_t someSize(std::mt19937& rng)
{
    const unsigned roll = rng() % 100;
    if (roll < 60)
        return rng() % 2048;
    if (roll < 90)
        return rng() % (96 * 1024);
    return rng() % (900 * 1024);
}

void client(uint16_t port, unsigned seed, const std::atomic<bool>& stop, Counters& counters)
{
    t_client = static_cast<int>(seed - 1000);
    std::mt19937 rng(seed);
    const std::string filler(1024 * 1024, 'x');
    std::string pending;
    while (!stop.load()) {
        at(1);
        const sys::socket_t socket = connectTo(port);
        if (socket == sys::kInvalidSocket) {
            ++counters.errors;
            continue;
        }
        ++counters.connections;
        at(2);
        pending.clear();
        const unsigned kind = rng() % 10;
        bool ok = true;
        if (kind < 6) {
            // HTTP: a run of requests on one connection.
            const int rounds = 300 + static_cast<int>(rng() % 3000);
            for (int round = 0; ok && round < rounds && !stop.load(); ++round) {
                const unsigned what = rng() % 10;
                at(10 + static_cast<int>(what));
                if (what < 4) {
                    const size_t size = someSize(rng);
                    ok = sendAll(socket, "GET /bytes/" + std::to_string(size) + " HTTP/1.1\r\nHost: soak\r\n\r\n") &&
                         readResponse(socket, pending, size);
                } else if (what < 6) {
                    const size_t size = someSize(rng);
                    ok = sendAll(socket, "GET /pooled/" + std::to_string(size) + " HTTP/1.1\r\nHost: soak\r\n\r\n") &&
                         readResponse(socket, pending, size);
                } else if (what < 8) {
                    const size_t size = someSize(rng);
                    std::string request = "POST /upload HTTP/1.1\r\nHost: soak\r\nContent-Length: " + std::to_string(size) + "\r\n\r\n";
                    request.append(filler, 0, size);
                    ok = sendAll(socket, request) && readResponse(socket, pending, std::to_string(size).size());
                } else if (what < 9) {
                    // A pipelined burst.
                    std::string burst;
                    const int count = 2 + static_cast<int>(rng() % 30);
                    for (int i = 0; i < count; ++i)
                        burst += "GET /bytes/64 HTTP/1.1\r\nHost: soak\r\n\r\n";
                    ok = sendAll(socket, burst);
                    for (int i = 0; ok && i < count; ++i)
                        ok = readResponse(socket, pending, 64);
                    counters.requests += static_cast<uint64_t>(count - 1);
                } else if (rng() % 20 != 0) {
                    ok = sendAll(socket, "GET /bytes/1 HTTP/1.1\r\nHost: soak\r\n\r\n") && readResponse(socket, pending, 1);
                } else {
                    // Walk away in the middle of an upload.
                    sendAll(socket, "POST /upload HTTP/1.1\r\nHost: soak\r\nContent-Length: 500000\r\n\r\n" + filler.substr(0, 1000 + rng() % 100000));
                    ++counters.dropped;
                    break;
                }
                ++counters.requests;
            }
        } else {
            // WebSocket: echo frames, then close properly or just vanish.
            at(30);
            ok = sendAll(socket, "GET /ws HTTP/1.1\r\nHost: soak\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
            while (ok && pending.find("\r\n\r\n") == std::string::npos)
                ok = readUpTo(socket, pending, pending.size() + 1);
            if (ok)
                pending.erase(0, pending.find("\r\n\r\n") + 4);
            const int rounds = 300 + static_cast<int>(rng() % 2000);
            for (int round = 0; ok && round < rounds && !stop.load(); ++round) {
                const size_t size = someSize(rng);
                at(31);
                const std::string frame = clientFrame(http::WsOpcode::Binary, std::string_view(filler).substr(0, size));
                // The echo comes back while a large frame is still going out: write from another thread.
                std::thread writer([&] { sendAll(socket, frame); });
                size_t echoed = 0;
                ok = readFrame(socket, pending, echoed) && echoed == size;
                writer.join();
                ++counters.frames;
            }
            if (ok && rng() % 3 == 0) {
                at(32);
                // Vanish with half a frame on the wire.
                const std::string frame = clientFrame(http::WsOpcode::Binary, std::string_view(filler).substr(0, 200000));
                sendAll(socket, std::string_view(frame).substr(0, 1 + rng() % 150000));
                ++counters.dropped;
            } else if (ok) {
                at(33);
                sendAll(socket, clientFrame(http::WsOpcode::Close, std::string("\x03\xE8", 2)));
                size_t ignored = 0;
                readFrame(socket, pending, ignored);
            }
        }
        if (!ok)
            ++counters.errors;
        at(40);
        sys::closeSocket(socket);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 60;
    const int clients = argc > 2 ? std::atoi(argv[2]) : 6;

    http::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = freePort();
    config.ioThreads = 2;
    config.maxBodyBytes = 4 * 1024 * 1024;
    config.maxWebSocketMessageBytes = 4 * 1024 * 1024;
    config.maxWebSocketQueuedBytes = 16 * 1024 * 1024;
    http::Server server(config);
    const std::string filler(1024 * 1024, 'y');
    server.onGet("/bytes/{size}", [&](http::HttpContext& context) {
        const size_t size = std::min<size_t>(filler.size(), std::strtoull(std::string(context.request().path.substr(7)).c_str(), nullptr, 10));
        context.text(std::string_view(filler).substr(0, size));
    });
    server.onGet("/pooled/{size}", [&](http::HttpContext& context) {
        const size_t size = std::min<size_t>(filler.size(), std::strtoull(std::string(context.request().path.substr(8)).c_str(), nullptr, 10));
        http::Buffer block(server.bufferPool(), size);
        block.append(filler.data(), size);
        context.bytes(std::move(block));
    });
    server.onPost("/upload", [](http::HttpContext& context) { context.text(std::to_string(context.request().body.size())); });
    server.onWebSocket("/ws", [&](std::shared_ptr<http::WebSocketConnection> connection) {
        http::WebSocketConnection* raw = connection.get();
        connection->onMessage([&server, raw](std::string_view message, bool binary) {
            if (message.size() >= 32 * 1024) {
                http::Buffer block(server.bufferPool(), message.size());
                block.append(message.data(), message.size());
                raw->send(std::move(block), binary);
            } else {
                raw->send(message, binary);
            }
        });
    });

    std::promise<void> listening;
    auto serving = std::async(std::launch::async, [&] { server.run({}, [&] { listening.set_value(); }); });
    listening.get_future().wait();

    std::atomic<bool> stop{false};
    Counters counters;
    std::vector<std::thread> threads;
    for (int i = 0; i < clients; ++i)
        threads.emplace_back(client, config.port, 1000u + static_cast<unsigned>(i), std::cref(stop), std::ref(counters));

    const auto started = std::chrono::steady_clock::now();
    size_t baseline = 0, last = 0, peak = 0;
    std::printf("%6s %12s %12s %10s %10s %8s %8s %12s %10s\n", "sec", "memory KiB", "pool KiB", "requests", "frames", "conns", "dropped", "retained KiB", "errors");
    for (int tick = 1; tick * 5 <= seconds; ++tick) {
        std::this_thread::sleep_until(started + std::chrono::seconds(tick * 5));
        last = memoryInUse();
        peak = std::max(peak, last);
        // The warm-up: a third of the run (every size class and every Session has seen its peak by then).
        if (!baseline && tick * 5 >= seconds / 3)
            baseline = last;
        const auto stats = server.bufferStats();
        std::printf("%6d %12zu %12zu %10llu %10llu %8llu %8llu %12zu %10llu\n", tick * 5, last / 1024, stats.backingBytes / 1024,
                    static_cast<unsigned long long>(counters.requests.load()), static_cast<unsigned long long>(counters.frames.load()),
                    static_cast<unsigned long long>(counters.connections.load()), static_cast<unsigned long long>(counters.dropped.load()),
                    server.stats().retainedBytes / 1024, static_cast<unsigned long long>(counters.errors.load()));
        std::printf("       connect failures %llu (last error %d); clients at:", static_cast<unsigned long long>(g_connectFailures.load()), g_lastConnectError.load());
        for (int i = 0; i < clients; ++i)
            std::printf(" %d", g_state[i].load());
        std::printf("\n");
        std::fflush(stdout);
    }
    stop = true;
    for (auto& thread : threads)
        thread.join();
    const auto stats = server.bufferStats();
    std::printf("outstanding pool blocks at the end: %zu\n", stats.outstandingBlocks);
    server.stop();
    serving.get();

    if (baseline && last > baseline + baseline / 4) {
        std::printf("FAILED: memory grew from %zu KiB after the warm-up to %zu KiB\n", baseline / 1024, last / 1024);
        return 1;
    }
    std::printf("ok: memory level (%zu KiB after the warm-up, %zu KiB at the end, peak %zu KiB)\n", baseline / 1024, last / 1024, peak / 1024);
    return 0;
}
