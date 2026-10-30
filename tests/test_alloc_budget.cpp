// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The allocation policy, enforced: once warmed up, the server side (reactor,
// worker and the handlers below, which are written to allocate nothing of
// their own) makes no heap allocation per request, per WebSocket message or
// per stream chunk; and a Session that served huge responses keeps no more
// than a small, fixed amount of memory once its connection is gone.
//
// The client side runs on threads marked as such; only the other threads'
// operator new calls are counted.

#include "http/http_context.hpp"
#include "http/server.hpp"
#include "http/websocket.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <new>
#include <string>
#include <thread>
#include <vector>

namespace {
thread_local bool t_client = false;
std::atomic<unsigned long> g_serverAllocations{0};
std::atomic<unsigned long> g_serverBytes{0};

void* counted(size_t size)
{
    if (!t_client) {
        g_serverAllocations.fetch_add(1, std::memory_order_relaxed);
        g_serverBytes.fetch_add(size, std::memory_order_relaxed);
    }
    if (void* pointer = std::malloc(size ? size : 1))
        return pointer;

    throw std::bad_alloc();
}
}  // namespace

void* operator new(size_t size)
{
    return counted(size);
}
void* operator new[](size_t size)
{
    return counted(size);
}
void* operator new(size_t size, const std::nothrow_t&) noexcept
{
    try {
        return counted(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](size_t size, const std::nothrow_t&) noexcept
{
    try {
        return counted(size);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* pointer) noexcept
{
    std::free(pointer);
}
void operator delete[](void* pointer) noexcept
{
    std::free(pointer);
}
void operator delete(void* pointer, size_t) noexcept
{
    std::free(pointer);
}
void operator delete[](void* pointer, size_t) noexcept
{
    std::free(pointer);
}

namespace {
constexpr int kWarmup = 100;
constexpr int kMeasured = 300;
constexpr size_t kChunkBytes = 10000;
constexpr size_t kPooledBytes = 2 * 1024 * 1024;
bool g_failed = false;

void fail(const char* what)
{
    std::fprintf(stderr, "FAILED: %s\n", what);
    std::_Exit(1);
}

uint16_t freePort()
{
    const int reservation = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(reservation, reinterpret_cast<sockaddr*>(&address), sizeof(address)))
        fail("bind");

    socklen_t size = sizeof(address);
    getsockname(reservation, reinterpret_cast<sockaddr*>(&address), &size);
    ::close(reservation);
    return ntohs(address.sin_port);
}

// From 127.0.0.100 when `source` is set: a peer address ("host:port") longer
// than a string keeps inline, as for any client on a LAN.
int connectTo(uint16_t port, const char* source)
{
    for (int attempt = 0; attempt < 2000; ++attempt) {
        const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
        if (source) {
            sockaddr_in local{};
            local.sin_family = AF_INET;
            inet_pton(AF_INET, source, &local.sin_addr);
            if (::bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)))
                fail("bind to the source address");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            timeval timeout{10, 0};
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            int one = 1;
            setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            return socket;
        }
        ::close(socket);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    fail("server did not start");
    return -1;
}

void sendAll(int socket, std::string_view data)
{
    while (!data.empty()) {
        const ssize_t sent = ::send(socket, data.data(), data.size(), MSG_NOSIGNAL);
        if (sent <= 0)
            fail("send");

        data.remove_prefix(static_cast<size_t>(sent));
    }
}

void receiveExactly(int socket, char* into, size_t size)
{
    while (size) {
        const ssize_t got = ::recv(socket, into, size, 0);
        if (got <= 0)
            fail("receive");

        into += got;
        size -= static_cast<size_t>(got);
    }
}

// One response with a Content-Length; returns its status, the body in `body`.
int readResponse(int socket, std::string& body)
{
    std::string head;
    char byte;
    while (head.size() < 4 || head.compare(head.size() - 4, 4, "\r\n\r\n") != 0) {
        receiveExactly(socket, &byte, 1);
        head.push_back(byte);
    }
    const size_t length = head.find("Content-Length: ");
    const size_t size =
        length == std::string::npos ? 0 : std::strtoul(head.c_str() + length + 16, nullptr, 10);
    body.resize(size);
    receiveExactly(socket, &body[0], size);
    return std::atoi(head.c_str() + 9);
}

void expectStatus(int socket, std::string& body, int expected = 200)
{
    if (readResponse(socket, body) != expected)
        fail("unexpected status");
}

template <class Operation>
void measure(const char* label, Operation operation)
{
    for (int index = 0; index < kWarmup; ++index)
        operation();

    const unsigned long allocations = g_serverAllocations.load();
    const unsigned long bytes = g_serverBytes.load();
    for (int index = 0; index < kMeasured; ++index)
        operation();

    const double perOperation = double(g_serverAllocations.load() - allocations) / kMeasured;
    const double bytesPerOperation = double(g_serverBytes.load() - bytes) / kMeasured;
    std::printf("%-46s %6.2f new/op %9.1f bytes/op\n", label, perOperation, bytesPerOperation);
    if (perOperation != 0)
        g_failed = true;
}

json::Json detections(int seed)
{
    json::Json items = json::Json::array();
    for (int index = 0; index < 4; ++index)
        items.push_back(json::Json{{"plate", "ABC1234"},
                                   {"confidence", 0.93 + seed % 7 * 0.001},
                                   {"x1", 0.1},
                                   {"y1", 0.2},
                                   {"x2", 0.5},
                                   {"y2", 0.6}});
    return json::Json{
        {"status", "ok"}, {"count", 4}, {"elapsedMs", 12.5}, {"results", std::move(items)}};
}

// An answer a handler leaves to a worker, in storage made once: the handler,
// the worker and the reactor task each capture a single reference.
struct Pending
{
    http::HttpContext* context = nullptr;
    std::function<void()> done;
    json::Json answer;
    http::Buffer body;
    int seed = 0;
};

// Pushes stream chunks built on a thread of its own into a reused string.
struct Feeder
{
    std::shared_ptr<http::StreamingConnection> connection;
    std::string chunk;
    std::atomic<int> wanted{0};
    std::atomic<bool> stop{false};

    void push()
    {
        // A chunk is dropped while the previous one is still being written:
        // try again on the next loop round.
        if (!connection->write(std::move(chunk)))
            connection->post([this] { push(); });
    }
};

void runBudget()
{
    http::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = freePort();
    config.ioThreads = 1;
    config.workerThreads = 1;
    http::Server server(config);

    server.onGet("/ping", [](http::HttpContext& context) { context.text("pong"); });

    static http::IntParameter fps{"fps"};
    static http::FloatParameter quality{"quality"};
    static http::StringParameter detect{"detect"};
    server.onGet("/api/camera/{camera}/frames", [](http::HttpContext& context) {
        context.parseQuery({&fps, &quality, &detect});
        if (context.routeParam("camera") != "7" || fps.value() != 30 || quality.value() != 0.5f ||
            detect.value() != "plate,vehicle,face,person" ||
            context.remoteAddress().rfind("127.0.0.100:", 0) != 0)
            context.status(400);

        context.text("ok");
    });

    static Pending work;
    server.onPost("/work", [&server](http::HttpContext& context, std::function<void()> done) {
        work.context = &context;
        work.done = std::move(done);
        server.pool().tryEnqueue([] {
            // Built on the worker, destroyed on the reactor.
            work.answer = detections(++work.seed);
            work.context->loop().tryPost([] {
                work.context->json(work.answer);
                work.answer = json::Json();
                work.done();
            });
        });
    });

    static json::Json numbers = json::Json::array();
    for (int index = 0; index < 1000; ++index)
        numbers.push_back(index * 0.001 + 0.0625 / (index + 1));
    server.onGet("/doubles", [](http::HttpContext& context) { context.json(numbers); });

    static Pending binary;
    server.onGet("/pooled", [&server](http::HttpContext& context, std::function<void()> done) {
        binary.context = &context;
        binary.done = std::move(done);
        server.pool().tryEnqueue([&server] {
            // Encoded straight into a block of the server's pool, as a JPEG
            // encoder would, and sent from it.
            http::Buffer body(server.bufferPool(), kPooledBytes);
            std::memset(body.data(), 'J', kPooledBytes);
            body.setSize(kPooledBytes);
            binary.body = std::move(body);
            binary.context->loop().tryPost([] {
                binary.context->bytes(std::move(binary.body), "image/jpeg");
                binary.done();
            });
        });
    });

    static std::shared_ptr<http::WebSocketConnection> socket;
    server.onWebSocket("/ws", [](std::shared_ptr<http::WebSocketConnection> connection) {
        socket = connection;
        http::WebSocketConnection* raw = connection.get();
        connection->onMessage(
            [raw](std::string_view data, bool binary) { raw->send(data, binary); });
    });

    // A big frame sent from its pooled block (send(Buffer&&)): no copy, no heap.
    static http::BufferPool* wsPool = &server.bufferPool();
    server.onWebSocket("/wsblob", [](std::shared_ptr<http::WebSocketConnection> connection) {
        http::WebSocketConnection* raw = connection.get();
        connection->onMessage([raw](std::string_view, bool) {
            http::Buffer frame(*wsPool, 128 * 1024);
            std::memset(frame.data(), 'W', 128 * 1024);
            frame.setSize(128 * 1024);
            raw->send(std::move(frame), true);
        });
    });

    static Feeder feeder;
    std::promise<void> streaming;
    server.onStream("/mjpeg", [&streaming](http::HttpContext& context,
                                           std::shared_ptr<http::StreamingConnection> connection) {
        context.header("Content-Type", "multipart/x-mixed-replace; boundary=frame");
        feeder.connection = connection;
        streaming.set_value();
    });

    auto serving = std::async(std::launch::async, [&] { server.run(); });
    std::thread feeding([] {
        int served = 0;
        while (!feeder.stop) {
            if (feeder.wanted.load() == served) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;
            }
            feeder.chunk.assign("--frame\r\nContent-Type: image/jpeg\r\n\r\n");
            feeder.chunk.append(kChunkBytes - feeder.chunk.size(),
                                static_cast<char>('a' + served % 26));
            ++served;
            feeder.connection->post([] { feeder.push(); });
        }
    });

    t_client = true;
    std::string body;
    const int client = connectTo(config.port, "127.0.0.100");
    measure("keep-alive GET", [&] {
        sendAll(client, "GET /ping HTTP/1.1\r\nHost: x\r\nUser-Agent: test\r\nAccept: */*\r\n\r\n");
        expectStatus(client, body);
    });
    measure("GET, 3 query + 1 route parameter, LAN peer", [&] {
        sendAll(client,
                "GET /api/camera/7/frames?fps=30&quality=0.5&detect=plate%2Cvehicle%2Cface%2Cperson"
                " HTTP/1.1\r\nHost: x\r\n\r\n");
        expectStatus(client, body);
    });
    std::string post =
        "POST /work HTTP/1.1\r\nHost: x\r\nContent-Type: application/octet-stream\r\n"
        "Content-Length: 1048576\r\n\r\n";
    post.append(1048576, 'x');
    measure("POST 1 MiB, Json answer built on a worker", [&] {
        sendAll(client, post);
        expectStatus(client, body);
        if (body.find("ABC1234") == std::string::npos)
            fail("worker answer");
    });
    measure("GET, Json answer with 1000 doubles", [&] {
        sendAll(client, "GET /doubles HTTP/1.1\r\nHost: x\r\n\r\n");
        expectStatus(client, body);
    });
    measure("GET, 2 MiB pooled body from a worker", [&] {
        sendAll(client, "GET /pooled HTTP/1.1\r\nHost: x\r\n\r\n");
        expectStatus(client, body);
        if (body.size() != kPooledBytes || body.front() != 'J' || body.back() != 'J')
            fail("pooled body");
    });
    ::close(client);

    const int ws = connectTo(config.port, "127.0.0.100");
    sendAll(ws,
            "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
            "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    std::string handshake;
    char byte;
    while (handshake.find("\r\n\r\n") == std::string::npos) {
        receiveExactly(ws, &byte, 1);
        handshake.push_back(byte);
    }
    const char payload[] = "hello, websocket";
    const size_t length = sizeof(payload) - 1;
    char frame[6 + sizeof(payload)];
    frame[0] = static_cast<char>(0x81);
    frame[1] = static_cast<char>(0x80 | length);
    const unsigned char mask[4] = {1, 2, 3, 4};
    std::memcpy(frame + 2, mask, 4);
    for (size_t index = 0; index < length; ++index)
        frame[6 + index] = static_cast<char>(payload[index] ^ mask[index % 4]);
    measure("WebSocket echo", [&] {
        sendAll(ws, std::string_view(frame, 6 + length));
        char echo[2 + sizeof(payload)];
        receiveExactly(ws, echo, 2 + length);
        if (std::memcmp(echo + 2, payload, length) != 0)
            fail("echo");
    });
    ::close(ws);

    const int blob = connectTo(config.port, "127.0.0.100");
    sendAll(blob,
            "GET /wsblob HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
            "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    std::string blobHead;
    while (blobHead.find("\r\n\r\n") == std::string::npos) {
        receiveExactly(blob, &byte, 1);
        blobHead.push_back(byte);
    }
    std::vector<char> blobBody(10 + 128 * 1024);
    measure("WebSocket 128 KiB frame from a pooled block", [&] {
        sendAll(blob, std::string_view(frame, 6 + length));
        receiveExactly(blob, blobBody.data(), 10 + 128 * 1024);  // 2 + 8 byte header
        if (blobBody[10] != 'W' || blobBody[10 + 128 * 1024 - 1] != 'W')
            fail("pooled frame");
    });
    ::close(blob);

    const int viewer = connectTo(config.port, "127.0.0.100");
    sendAll(viewer, "GET /mjpeg HTTP/1.1\r\nHost: x\r\n\r\n");
    std::string head;
    while (head.find("\r\n\r\n") == std::string::npos) {
        receiveExactly(viewer, &byte, 1);
        head.push_back(byte);
    }
    streaming.get_future().wait();
    std::vector<char> chunk(kChunkBytes);
    int expected = 0;
    measure("MJPEG chunk pushed from another thread", [&] {
        ++feeder.wanted;
        receiveExactly(viewer, chunk.data(), chunk.size());
        if (chunk.back() != static_cast<char>('a' + expected++ % 26))
            fail("stream chunk");
    });
    ::close(viewer);

    feeder.stop = true;
    feeding.join();
    server.stop();
    serving.get();
    feeder.connection.reset();
    socket.reset();
    t_client = false;
}

// A Session's buffers grow to whatever its connection needed; once the
// connection is gone they are cut back, whatever the largest message was.
void runRetention()
{
    http::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = freePort();
    config.ioThreads = 1;
    config.workerThreads = 1;
    http::Server server(config);
    static const std::string big(20 * 1000 * 1000, 'b');
    server.onGet("/big", [](http::HttpContext& context) { context.text(big); });
    auto serving = std::async(std::launch::async, [&] { server.run(); });

    // Four connections at a time, so that four Sessions serve them; 32
    // responses of 20 MB in all.
    constexpr size_t kConnections = 4;
    constexpr int kResponses = 32;
    t_client = true;
    std::vector<int> clients;
    for (size_t index = 0; index < kConnections; ++index)
        clients.push_back(connectTo(config.port, nullptr));

    std::string body;
    for (int index = 0; index < kResponses; ++index) {
        const int client = clients[static_cast<size_t>(index) % kConnections];
        sendAll(client, "GET /big HTTP/1.1\r\nHost: x\r\n\r\n");
        expectStatus(client, body);
        if (body.size() != big.size())
            fail("big body");
    }
    if (server.stats().retainedBytes != 0)
        fail("nothing is released while the connections are open");

    for (int client : clients)
        ::close(client);

    // Every Session keeps at least its two 1 KiB output reserves.
    size_t retained = 0;
    for (int wait = 0; wait < 200; ++wait) {
        retained = server.stats().retainedBytes;
        if (retained >= kConnections * 2048)
            break;

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::printf("retained after %d x 20 MB over %zu Sessions: %zu bytes\n", kResponses,
                kConnections, retained);
    if (retained < kConnections * 2048 || retained > kConnections * 64 * 1024)
        fail("retained bytes after the connections closed");

    server.stop();
    serving.get();
    t_client = false;
}
}  // namespace

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    runBudget();
    if (g_failed)
        fail("server-side allocations after warm-up");

    runRetention();
    std::puts(
        "allocation budget: no server-side allocation per request, message or chunk; "
        "bounded Session retention passed");
}
