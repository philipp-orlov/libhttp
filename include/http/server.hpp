// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "http/io_loop.hpp"
#include "http/memory_pool.hpp"
#include "http/route_pattern.hpp"
#include "http/router.hpp"
#include "http/static_files.hpp"
#include "http/streaming_connection.hpp"
#include "http/thread_pool.hpp"
#include "http/websocket_connection.hpp"

namespace http {

class Session;
namespace internal {
struct ServerTls;
struct SessionPool;
}

struct TlsConfig
{
    uint16_t port = 8443;
    std::string certificateFile;
    std::string privateKeyFile;
};

struct ServerConfig
{
    std::string host = "0.0.0.0";
    uint16_t port = 8080;
    bool httpEnabled = true;
    std::optional<TlsConfig> tls;

    // 0 => hardware_concurrency() reactor threads, each with its own
    // epoll/IOCP loop and (on Linux) its own SO_REUSEPORT accept queue.
    unsigned ioThreads = 0;

    // Worker threads for Server::pool(), used by handlers to offload long or
    // CPU-bound work off the reactor threads.
    // 0 => 2 threads. Created by pool() or before run() accepts connections.
    unsigned workerThreads = 0;
    size_t workerQueueCapacity = 128;
    size_t reactorTaskCapacity = 1024;
    size_t reactorTimerCapacity = 4096;
    size_t maxConnectionsPerLoop = 256;
    // Concurrent connections one client address may hold on one reactor
    // (connections spread over ioThreads loops, so the server-wide cap is up
    // to ioThreads times this). 0 = unlimited.
    size_t maxConnectionsPerIp = 0;
    size_t maxWebSocketMessageBytes = 1024 * 1024;
    size_t maxWebSocketQueuedBytes = 4 * 1024 * 1024;
    size_t maxWebSocketQueuedFrames = 256;
    BufferPool::Config buffers;

    size_t maxHeaderBytes = 64 * 1024;
    size_t maxBodyBytes = 256ull * 1024 * 1024;  // generous cap for image uploads
    int keepAliveTimeoutSeconds = 75;
    int requestTimeoutSeconds = 120;
    // A client has this long, from its first byte (or from accept), to
    // finish the request line and headers; the rest of the request is
    // bounded by requestTimeoutSeconds. 0 disables.
    int headerReadTimeoutSeconds = 10;
    bool reusePort = true;
    std::string serverHeader = "ws";

    // Value echoed in Access-Control-Allow-Origin on every response, and the
    // switch for preflight (OPTIONS) CORS headers. Empty disables CORS: a
    // browser on another origin then cannot call the API at all.
    std::string corsAllowOrigin;

    // Host names (no port; IPv6 literals in brackets) this server answers
    // to, compared case-insensitively against the Host header. A request for
    // any other name gets 421, which is what stops a DNS-rebinding page from
    // talking to a service bound to a private address. Empty accepts any
    // Host -- unsafe on anything but a trusted network.
    std::vector<std::string> allowedHosts;

    // Origins ("https://ops.example:8443") allowed to open a WebSocket, which
    // browsers do not confine by the same-origin policy. A handshake carrying
    // another Origin gets 403; one with no Origin (not a browser) is let
    // through. Empty accepts any Origin.
    std::vector<std::string> allowedOrigins;

    // Request headers a CORS preflight may be granted; others named in
    // Access-Control-Request-Headers are left out of the reply.
    std::vector<std::string> corsAllowedHeaders = {"Content-Type", "Authorization",
                                                   "X-Requested-With", "Accept"};
};

// Fluent HTTP + WebSocket server:
//   Server server(config);
//   server.onGet("/health", [](HttpContext& context){ context.json({{"status","ok"}}); })
//         .onPost("/upload/{name}", [](HttpContext& context, std::function<void()> done){ ... })
//         .onStaticFiles("/static", "./public")
//         .onWebSocket("/ws", [](std::shared_ptr<WebSocketConnection> ws){ ... });
//   server.run();
class Server
{
public:
    explicit Server(ServerConfig config = {});
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    template <class F>
    Server& onGet(std::string pattern, F&& handlerFunction, RouteDefaults defaultValues = {})
    {
        router_.onGet(std::move(pattern), std::forward<F>(handlerFunction),
                      std::move(defaultValues));
        return *this;
    }
    template <class F>
    Server& onHead(std::string pattern, F&& handlerFunction, RouteDefaults defaultValues = {})
    {
        router_.onHead(std::move(pattern), std::forward<F>(handlerFunction),
                       std::move(defaultValues));
        return *this;
    }
    template <class F>
    Server& onPost(std::string pattern, F&& handlerFunction, RouteDefaults defaultValues = {})
    {
        router_.onPost(std::move(pattern), std::forward<F>(handlerFunction),
                       std::move(defaultValues));
        return *this;
    }
    template <class F>
    Server& onPut(std::string pattern, F&& handlerFunction, RouteDefaults defaultValues = {})
    {
        router_.onPut(std::move(pattern), std::forward<F>(handlerFunction),
                      std::move(defaultValues));
        return *this;
    }
    template <class F>
    Server& onPatch(std::string pattern, F&& handlerFunction, RouteDefaults defaultValues = {})
    {
        router_.onPatch(std::move(pattern), std::forward<F>(handlerFunction),
                        std::move(defaultValues));
        return *this;
    }
    template <class F>
    Server& onDelete(std::string pattern, F&& handlerFunction, RouteDefaults defaultValues = {})
    {
        router_.onDelete(std::move(pattern), std::forward<F>(handlerFunction),
                         std::move(defaultValues));
        return *this;
    }

    Server& onStaticFiles(std::string urlPrefix, std::string rootDir);
    Server& onStaticFiles(std::string urlPrefix, std::string rootDir,
                          StaticFileHandler::Options options);
    Server& onWebSocket(std::string pattern, WebSocketHandler handler);
    // GET only: a long-lived, server-push response (see streaming_connection.hpp).
    Server& onStream(std::string pattern, StreamHandler handler);

    // Replaces the built-in `404 Not Found` body for a path no route and no
    // static handler claimed. Useful when routes are registered from data
    // and a caller cannot guess them -- the handler can list what does exist.
    // The status is already set to 404 when it runs; 405 is not routed here,
    // since the path did match, only the method did not.
    Server& onNotFound(std::function<void(HttpContext&)> handler)
    {
        notFound_ = std::move(handler);
        return *this;
    }

    // Lazily creates (on first call) and returns the worker thread
    // pool, sized from config_.workerThreads (default 2 if unset).
    ThreadPool& pool();

    // The caller coordinates shutdown; reactors run on dedicated threads.
    // shouldStop is polled on the caller, suitable for a sig_atomic_t flag.
    void run(std::function<bool()> shouldStop = {});
    void stop();

    const ServerConfig& config() const { return config_; }
    BufferPool::Stats bufferStats() const { return buffers_.stats(); }

    // Work handed to the bounded queues that did not run, for the operator:
    // a nonzero rejected count means completions or posted tasks were lost
    // (size reactorTaskCapacity >= workerQueueCapacity + workers, and check
    // the result of IoLoop::tryPost / ThreadPool::tryEnqueue); failed counts
    // tasks that threw. Callable from any thread once run() has started.
    struct Stats
    {
        size_t reactorTasksFailed = 0;
        size_t reactorTasksRejected = 0;
        size_t reactorQueueDepth = 0;  // summed over the loops
        size_t workerTasksFailed = 0;
        size_t workerTasksRejected = 0;
        size_t workerQueueDepth = 0;
    };
    Stats stats() const;

private:
    friend class Session;

    void runLoop(size_t index);
    void handleAccept(size_t index, socket_t fd, bool tls = false);
    void releaseSession(size_t index, Session& session);
    void connectionClosed(size_t index, const std::string& peerIp);

    ServerConfig config_;
    // "Connection: ...\r\nServer: ...\r\n[CORS]\r\n": the fixed end of every
    // response head, assembled once (see Session::serializeHead).
    std::string headTailKeepAlive_;
    std::string headTailClose_;
    std::unique_ptr<internal::ServerTls> tls_;
    BufferPool buffers_;
    Router router_;
    std::vector<StaticFileHandler> staticHandlers_;
    std::function<void(HttpContext&)> notFound_;
    RouteTable<WebSocketHandler> webSocketRoutes_;
    RouteTable<StreamHandler> streamRoutes_;

    mutable std::mutex poolMutex_;
    std::unique_ptr<ThreadPool> pool_;

    std::vector<std::unique_ptr<IoLoop>> loops_;
    std::atomic<bool> loopsReady_{false};  // loops_ is complete and no longer changes
    std::vector<std::unique_ptr<TcpListener>> listeners_;
    std::vector<std::unique_ptr<TcpListener>> tlsListeners_;
    std::vector<std::thread> threads_;
    // One pool per loop: sessions are recycled across connections rather than
    // allocated per accept (see Session in server.cpp).
    std::vector<std::unique_ptr<internal::SessionPool>> sessionPools_;
    std::mutex stopMutex_;
    std::condition_variable stopCv_;
    size_t closedListeners_ = 0;
    std::atomic<bool> running_{false};
};

}  // namespace http
