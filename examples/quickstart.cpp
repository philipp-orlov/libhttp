// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include <csignal>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <thread>

#include "http/logger.hpp"
#include "http/memory_pool.hpp"
#include "http/server.hpp"
#include "json/json.hpp"

using namespace http;
using json::Json;

namespace {
volatile std::sig_atomic_t stopRequested = 0;
void handleSignal(int)
{
    stopRequested = 1;
}

// Demonstrates routing to a member function instead of a lambda, and the
// "/api/{controller}/{action}" style routing ASP.NET MVC is known for,
// with "action" made optional via a default value passed at registration
// time (see main(): onGet(..., {{"action", "index"}})) rather than any
// special syntax in the pattern string itself.
class ApiController
{
public:
    void handleRequest(HttpContext& context)
    {
        std::string controllerName(context.routeParam("controller"));
        std::string actionName(context.routeParam("action"));
        context.json(Json{
            {"controller", controllerName},
            {"action", actionName},
        });
    }
};

// Demonstrates a binary-in/binary-out endpoint, and an async member
// function handler that offloads work to the server's worker thread pool
// -- the shape any heavy transform takes: an image comes in
// as a raw binary body, the (stand-in) transform runs off the reactor thread,
// and a processed binary result goes back out.
class InferenceController
{
public:
    explicit InferenceController(Server& server) : server_(server) {}

    void handleInfer(HttpContext& context, std::function<void()> done)
    {
        std::string modelName(context.routeParam("model"));
        std::string imageBytes(context.request().body.view());  // raw binary request body
        IoLoop& loop = context.loop();

        server_.pool().enqueue([&context, &loop, modelName, imageBytes = std::move(imageBytes),
                                done = std::move(done)]() mutable {
            // Placeholder for the real transform. Runs on a worker thread,
            // not the reactor thread, so a slow one never blocks I/O for
            // other connections. The stand-in "processing" inverts every
            // byte, producing a binary result the same size as the input.
            std::string processedBytes(imageBytes.size(), '\0');
            for (size_t i = 0; i < imageBytes.size(); ++i) {
                processedBytes[i] = static_cast<char>(~static_cast<unsigned char>(imageBytes[i]));
            }

            // Hop back onto the connection's own IoLoop before touching
            // context/the socket -- HttpContext is not thread-safe to
            // touch from a worker thread directly. `done` keeps the
            // connection's session alive (and context valid) until this
            // fires.
            loop.post([&context, modelName, processedBytes = std::move(processedBytes),
                       done = std::move(done)]() mutable {
                context.header("X-Model", modelName);
                context.bytes(processedBytes.data(), processedBytes.size(),
                              "application/octet-stream");
                done();
            });
        });
    }

private:
    Server& server_;
};

// A fixed-size raw grayscale image buffer, 1 byte/pixel -- the shape a
// capture or decode pipeline hands you, not a string. Used by the "/ws/image"
// WebSocket endpoint below to demonstrate binary WebSocket I/O via a
// pointer + length rather than string_view.
constexpr size_t kImageWidth = 64;
constexpr size_t kImageHeight = 64;
constexpr size_t kImageBufferSize = kImageWidth * kImageHeight;
}  // namespace

int main(int argc, char** argv)
try {
    Pal pal;

    // Installs a custom allocator on the buffer pool -- e.g. for zero-copy
    // handoff on a system with memory shared between the CPU and an
    // accelerator, where one buffer is visible to both the CPU (this reactor
    // thread, filling it from the socket) and the device reading it directly,
    // with no copy in between. The two lambdas below stand in for such an
    // allocator with plain heap calls -- swap their bodies for the real ones
    // on target hardware. Must be set before any request touches the pool, so
    // it's done here before the Server (and its I/O/worker threads) exists.
    BufferPool::setAllocator(
        [](size_t size) -> void* {
            return std::malloc(size);  // e.g. a shared-memory allocation
        },
        [](void* ptr, size_t /*size*/) {
            std::free(ptr);  // e.g. the matching release call
        });

    Logger::instance().setLevel(LogLevel::Info);
    Logger::instance().setLogFile("server.log");

    ServerConfig config;
    config.port = 8080;
    config.ioThreads = 4;
    config.workerThreads = 2;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--help") {
            std::puts(
                "http_example [--port PORT] [--https-port PORT] [--tls-cert PEM] [--tls-key PEM] "
                "[--https-only]");
            return 0;
        }
        if (option == "--https-only") {
            config.httpEnabled = false;
            continue;
        }
        if (option != "--port" && option != "--https-port" && option != "--tls-cert" &&
            option != "--tls-key")
            throw std::invalid_argument("unknown argument: " + std::string(option));

        if (++index == argc)
            throw std::invalid_argument("missing value for " + std::string(option));

        const std::string_view value(argv[index]);
        if (option != "--port" && !config.tls)
            config.tls.emplace();

        if (option == "--tls-cert") {
            config.tls->certificateFile = value;
        } else if (option == "--tls-key") {
            config.tls->privateKeyFile = value;
        } else {
            unsigned port = 0;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                port == 0 || port > 65535)
                throw std::invalid_argument("port must be within [1,65535]");

            if (option == "--port")
                config.port = static_cast<uint16_t>(port);
            else
                config.tls->port = static_cast<uint16_t>(port);
        }
    }

    Server server(config);
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    ApiController apiController;
    InferenceController inferenceController(server);

    // Simple synchronous handler: (HttpContext&) -> void.
    server.onGet("/health", [](HttpContext& context) { context.json(Json{{"status", "ok"}}); });

    // Query params + cookies, ASP.NET-HttpContext-style access.
    server.onGet("/greet", [](HttpContext& context) {
        std::string name(context.hasQuery("name") ? context.query("name") : "world");
        int visits = 1;
        if (context.hasCookie("visits")) {
            visits = std::atoi(std::string(context.cookie("visits")).c_str()) + 1;
        }
        CookieOptions cookie;
        cookie.name = "visits";
        cookie.value = std::to_string(visits);
        cookie.maxAgeSeconds = 3600;
        context.setCookie(cookie);
        context.json(Json{{"message", "hello, " + name}, {"visits", visits}});
    });

    // queryParams() -- iterate every query parameter without knowing their
    // names up front (query()/hasQuery() are for when you *do* know the
    // name). e.g. GET /search?q=widgets&sort=price&sort=rating
    server.onGet("/search", [](HttpContext& context) {
        Json params = Json::array();
        for (auto& [name, value] : context.queryParams()) {
            params.push_back(Json{{"name", name}, {"value", value}});
        }
        context.json(Json{{"params", params}});
    });

    // Returning a specific HTTP error code with a custom message: chain
    // status()/text() the same way you'd chain status codes in ASP.NET,
    // instead of always answering 200. Here a duplicate "id" query
    // parameter is treated as a conflict:
    //   GET /orders?id=1        -> 200 OK
    //   GET /orders?id=1&id=1   -> 409 Conflict, body "CUSTOM CONFLICT MESSAGE"
    server.onGet("/orders", [](HttpContext& context) {
        int idCount = 0;
        for (auto& [name, value] : context.queryParams()) {
            if (name == "id")
                ++idCount;
        }
        if (idCount > 1) {
            context.status(409).text("CUSTOM CONFLICT MESSAGE");
            return;
        }
        context.json(Json{{"status", "ok"}});
    });

    // "/api/{controller}/{action}" with "action" optional (defaults to
    // "index" when the URL omits it), dispatched to a member function via
    // std::bind rather than a lambda:
    //   GET /api/products/list -> controller="products", action="list"
    //   GET /api/products      -> controller="products", action="index"
    server.onGet("/api/{controller}/{action}",
                 std::bind(&ApiController::handleRequest, &apiController, std::placeholders::_1),
                 {{"action", "index"}});

    // Binary image in, binary result out; async member function handler
    // offloading to the worker pool. Route params still work the same way:
    //   POST /infer/invert  (body = raw image bytes)
    server.onPost("/infer/{model}",
                  std::bind(&InferenceController::handleInfer, &inferenceController,
                            std::placeholders::_1, std::placeholders::_2));

    server.onStaticFiles("/", "./public");

    server.onWebSocket("/ws", [](std::shared_ptr<WebSocketConnection> connection) {
        connection->onMessage([connection](std::string_view data, bool isBinary) {
            connection->send(data, isBinary);  // echo
        });
        connection->onClose([]() { HTTP_LOG_INFO("WebSocket connection closed"); });
    });

    // Binary WebSocket I/O via a raw buffer of a known size -- as if
    // receiving/returning a fixed-size image (64x64, 1 byte/pixel) rather
    // than a string_view. The frame still arrives as a std::string_view
    // (that's the wire-level byte span), but it's immediately treated as
    // a raw pointer + length -- the same shape a capture or decode pipeline
    // would hand you -- and sent back through the send(const void*, size_t, bool)
    // overload instead of forwarding the string_view.
    server.onWebSocket("/ws/image", [](std::shared_ptr<WebSocketConnection> connection) {
        connection->onMessage([connection](std::string_view data, bool isBinary) {
            if (!isBinary || data.size() != kImageBufferSize) {
                HTTP_LOG_WARN("expected a %zu-byte binary image frame, got %zu bytes (binary=%d)",
                              kImageBufferSize, data.size(), static_cast<int>(isBinary));
                return;
            }

            const unsigned char* imageIn = reinterpret_cast<const unsigned char*>(data.data());
            unsigned char imageOut[kImageBufferSize];
            for (size_t i = 0; i < kImageBufferSize; ++i) {
                imageOut[i] = static_cast<unsigned char>(~imageIn[i]);  // stand-in "processing"
            }

            connection->send(imageOut, kImageBufferSize, /*binary=*/true);
        });
        connection->onClose([]() { HTTP_LOG_INFO("Image WebSocket connection closed"); });
    });

    server.run([] { return stopRequested != 0; });
    Logger::instance().flush();
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
