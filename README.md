# libhttp

A small, embeddable HTTP/1.1 and WebSocket server for C++17. A reactor per
thread -- `epoll` on Linux, IOCP on Windows -- behind a fluent router, with TLS,
static files, typed query parameters and a worker pool for the work that must
not run on an I/O thread.

```cpp
#include "http/server.hpp"
#include "json/json.hpp"

int main()
{
    http::ServerConfig config;
    config.port = 8080;

    http::Server server(config);

    server.onGet("/health", [](http::HttpContext& context) {
              context.json(json::Json{{"status", "ok"}});
          })
          .onGet("/api/{controller}/{action}", [](http::HttpContext& context) {
              context.json(json::Json{{"controller", context.routeParam("controller")},
                                      {"action", context.routeParam("action")}});
          }, {{"action", "index"}})   // trailing parameter made optional
          .onStaticFiles("/static", "./public")
          .onWebSocket("/ws", [](std::shared_ptr<http::WebSocketConnection> socket) {
              socket->onMessage([socket](std::string_view data, bool binary) {
                  socket->send(data, binary);
              });
          });

    server.run();
}
```

## What you get

- **A reactor per thread.** Each I/O thread owns its own event loop and, on
  Linux, its own `SO_REUSEPORT` accept queue, so connections spread across
  threads without a shared accept lock. Handlers run on the loop that owns the
  connection, so nothing in a handler needs a mutex.
- **Routing that reads like the URL.** `"/api/{controller}/{action}"` with
  per-route default values for trailing parameters, a handler per verb, and a
  `onNotFound` hook for listing what does exist when routes are registered from
  data.
- **Synchronous or deferred, per handler.** Take an `HttpContext&` and answer
  now, or take an extra `std::function<void()> done`, hand the work to
  `Server::pool()`, hop back onto the connection's loop and answer then. The
  session and its context stay alive until `done` fires.
- **Typed query parameters.** Declare a struct of `IntParameter`,
  `FloatParameter`, `StringParameter` (with a choice list), `SizeArrayParameter`
  and so on with ranges and defaults; `bindQuery` parses, validates and answers
  `400` with a readable message before your handler runs.
- **WebSockets** with per-message and per-connection byte and frame caps,
  optional **TLS** (OpenSSL, opt-in) alongside or instead of plain HTTP,
  **static files** with content types, ETags and `304` conditional replies,
  **cookies**, and a **CORS** switch that is one config field.
- **Bounded everywhere.** Header and body caps, keep-alive and request
  timeouts, connections per loop, worker queue depth, reactor task and timer
  slots: every queue has a size and a defined behavior when it is full, which
  is what keeps a busy server answering instead of swelling.
- **Pooled buffers, pooled sessions.** Connection buffers come from a shared
  pool with observable statistics (`Server::bufferStats()`) rather than from
  the allocator on every request; the per-connection session objects are
  recycled across connections, and the buffers a connection's responses and
  WebSocket frames are assembled in stay with it, so a server answering at
  request rate for months does not take a fresh block from the heap per
  request or per message -- the steady-state request path allocates nothing.
- **Fast.** One `recv` and one `send` per request and no `epoll_ctl` on the
  request path; pipelined requests are answered in one write. On a single
  core it serves within a few percent of uWebSockets, and it scales to over
  ten million small requests per second on one 24-core socket -- see
  Performance below.
- **Static by default.** C++17, Linux and Windows, no third-party dependency
  in the default build (OpenSSL is opt-in, for TLS only -- see Building
  below; the WebSocket handshake digest is a self-contained SHA-1 and never
  needed it), and a bundled JSON value/parser/serializer (`json::Json`,
  `#include "json/json.hpp"`) for bodies. The example links fully static
  when built with TLS and the OpenSSL archives are installed, so it drops
  onto a target device without a matching toolchain.

## JSON

`json::Json` (`include/json/json.hpp`, `src/json.cpp`) is a small,
self-contained JSON value/parser/serializer built directly into `http` --
no separate library or link line needed, just `#include "json/json.hpp"`.

```cpp
json::Json body{
    {"status", "ok"},
    {"visits", 3},
    {"tags", json::Json::array()},
};
body["tags"].push_back("fast");

std::string out;              // a buffer the caller keeps and reuses
body.dump(out);               // {"status":"ok","visits":3,"tags":["fast"]}

json::Json parsed;
std::string error;
if (!json::Json::parse(out, parsed, error))
    std::puts(error.c_str()); // "JSON parse error at line 1, column 7: ..."
else if (const json::Json* status = parsed.find("status"))
    std::puts(status->asString().c_str());
```

- **One value type.** `Json` is a variant over null, bool, double, string,
  array and object, with implicit constructors for the types you already have
  and `asBool`/`asDouble`/`asInt`/`asString`/`asArray`/`asObject` to get them
  back out. Numbers are `double`, as JSON itself defines only one numeric type;
  integers up to 2^53 are exact.
- **Order is preserved.** Objects and arrays are flat vectors, so a document
  comes out of `dump()` in the order it was built or parsed, not sorted by key.
- **A parser that says no.** One pass over a fully buffered string, with errors
  carrying a 1-based line and column, and hard limits on everything an untrusted
  body can inflate: 16 MiB per document, 64 levels of nesting, 100k nodes, 4096
  members per object, 128 characters per number. Duplicate keys, unpaired
  surrogates, control characters in strings and malformed UTF-8 are all
  rejected rather than repaired.
- **Buffers that come back.** String, array and object buffers are recycled
  through a per-thread, byte-budgeted pool as values are destroyed and rebuilt,
  so a long-lived service settles on a fixed set of blocks instead of
  fragmenting the heap. `Json::trimCache()` hands that memory back after an
  unusually large document.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

No third-party dependency is needed for the default build: TLS is **off**
by default (`HTTP_WITH_SSL=OFF`) to keep the dependency list at zero --
plain HTTP and WebSocket (whose handshake digest is its own self-contained
SHA-1, never OpenSSL's) work exactly the same either way, and the bundled
JSON value/parser/serializer never needed a dependency in the first place.

To enable TLS, install OpenSSL 1.1.1 or newer and turn the option on:

```sh
sudo apt install libssl-dev        # Debian/Ubuntu
# or: sudo dnf install openssl-devel     (Fedora/RHEL)
# or: brew install openssl@3             (macOS)

cmake -S . -B build -DHTTP_WITH_SSL=ON
cmake --build build -j
```

With `HTTP_WITH_SSL=ON`, `ServerConfig::tls` works as documented (see the
top of this README); with it left `OFF`, setting `ServerConfig::tls` throws
`std::runtime_error` at startup instead of failing to link, so the mistake
surfaces immediately rather than silently.

Options: `HTTP_BUILD_TESTS`, `HTTP_BUILD_EXAMPLES`, `HTTP_WITH_SSL` (default
`OFF`), `HTTP_BUILD_BENCHMARKS` (default `OFF`; builds the load-test server
under `benchmarks/`, see Performance below), `HTTP_LTO` (default `OFF`;
link-time optimization for the library and the programs built here -- it
measured within noise on the request path, which is mostly kernel time),
`HTTP_WITH_IO_URING` (default `OFF`, Linux only; drives sockets through
io_uring instead of epoll -- needs liburing, see Performance below for what
it does and does not buy). The optimization level follows
`CMAKE_BUILD_TYPE`; a build configured without one still gets `-O2`.

Using it from another project, vendor it directly:

```cmake
add_subdirectory(libhttp)
target_link_libraries(my_app PRIVATE http)
```

## The example

`examples/quickstart.cpp` (built as `http_example`) serves `./public`,
the routes above, an echo WebSocket and a binary in/binary out endpoint that
offloads to the worker pool -- the shape any CPU-bound endpoint takes. Pass
`--tls-cert`/`--tls-key` to also listen on HTTPS -- that needs a build with
`-DHTTP_WITH_SSL=ON` (see Building above).

## Tests

`ctest` covers the request parser, the router's parameters, the memory pool, the
thread pool, the task and timer queues, the WebSocket framing, the TLS stream,
static file serving, the server end to end over a real socket (including
pipelined requests answered in order, a body too large to batch, an upgrade
pipelined behind a request, and bursts of WebSocket frames), and the bundled
JSON value/parser/serializer (finite numbers, duplicate keys, nesting depth,
UTF-8 and surrogate limits, round-tripping, and a 20k build/render/parse soak
test for leaks and heap growth).

## Performance

The request path is built to do the minimum the kernel requires and nothing
more:

- **epoll, edge-triggered, registered once.** A connection is added to epoll
  when accepted and removed when closed; `EPOLLOUT` is added only while a
  write is actually blocked. Readiness is tracked in flags, so a read issued
  right after a response waits for the next edge instead of paying for a
  `recv` that would only return `EAGAIN`. Steady state is one `recv` and one
  `send` per request, plus an `epoll_wait` amortized over every ready
  connection.
- **Corked output.** A response is appended to the connection's output buffer
  rather than sent on the spot; everything appended while the input at hand
  is being worked through goes to the socket in one `send`. A client that
  pipelines gets all its responses in one write and one segment; one that
  does not pays nothing for it. Output is bounded: past 64 KiB of corked
  bytes parsing pauses until the socket has caught up.
- **Nothing allocated per request.** Sessions are pooled per loop and recycled
  across connections, completion callbacks capture nothing but a pointer (so
  they live inside `std::function` without a heap block), request strings
  and header entries keep their storage between requests, and the response
  head is assembled in a stack buffer and appended once.
- **One receive buffer per loop.** Reads land in a 256 KiB buffer shared by
  every connection of the loop (each read is consumed before the next can
  complete) and are parsed in place; only a partial request or frame is
  copied to the connection. A WebSocket frame is unmasked in place and handed
  to the handler as a view; a large one is sent straight from that buffer
  with one `sendmsg`, copied only if the socket did not take all of it.
- **Coarse timers.** The idle timer is refreshed on every read and write but
  touches the timer heap only when it would move by more than a second.

### Measurements

`benchmarks/hello.cpp` (`-DHTTP_BUILD_BENCHMARKS=ON`, built as
`http_bench_hello [threads] [port]`) answers `GET /` with `Hello, World!`,
`GET /json` with a small JSON body, and echoes WebSocket messages on `/`.
The reference is uWebSockets (master, `-O3 -flto`, no TLS, no compression)
serving the same response from the same number of threads, each with its own
`SO_REUSEPORT` listener. Both ran on one socket of a 2x Xeon Gold 6252 (12
cores / 24 threads per socket, 2.1 GHz) with the load generator pinned to the
other socket, so the client competes with neither server for cores -- but it
does cap the non-pipelined multi-thread numbers, where both servers sit at
around 60% CPU waiting for it. Clients: bombardier 1.2.6 (keep-alive, no
pipelining), wrk 4.2 with `benchmarks/pipeline.lua` (16 requests per write),
and uWebSockets' own `benchmarks/load_test` for WebSocket echo.

| Load                                              | libhttp before | libhttp now | uWebSockets |
|---------------------------------------------------|---------------:|------------:|------------:|
| 1 thread, 64 conns, bombardier                    |    42k req/s   |   92k req/s |   96k req/s |
| 1 thread, 64 conns, wrk pipelined x16             |    56k req/s   |  749k req/s |  877k req/s |
| 12 threads, 256 conns, bombardier                 |   397k req/s   |  671k req/s |  668k req/s |
| 12 threads, 1024 conns, bombardier                |       --       |  714k req/s |  677k req/s |
| 12 threads, 256 conns, wrk pipelined x16          |   712k req/s   | 9.16M req/s | 9.27M req/s |
| 24 threads, 512 conns, wrk pipelined x16          |       --       | 11.6M req/s | 11.9M req/s |
| WebSocket echo, 1 thread, 64 conns, 20 B          |       --       |   92k msg/s |  100k msg/s |
| WebSocket echo, 1 thread, 64 conns, 1 KiB         |       --       |   91k msg/s |   90k msg/s |
| WebSocket echo, 1 thread, 512 conns, 16 KiB       |       --       |   49k msg/s |   47k msg/s |

"Before" is the level-triggered reactor with a write per response head and
body, a `getpeername` per request and a session allocated per connection. The
single-thread rows are the cleanest measure of per-request cost: the server
core is saturated, and around 90% of it is spent inside `recv`, `send` and
`epoll_wait`, which is why the remaining gap to uWebSockets is a few percent
and not more.

To reproduce, pin the server and the client to disjoint cores
(`benchmarks/benchmarks.md` has the full procedure, tool builds and the
uWebSockets reference setup):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHTTP_BUILD_BENCHMARKS=ON
cmake --build build -j
taskset -c 0-23 build/http_bench_hello 12 8080 &
taskset -c 24-47 bombardier -c 256 -d 10s --fasthttp http://127.0.0.1:8080/
taskset -c 24-47 wrk -t 12 -c 256 -d 10s -s benchmarks/pipeline.lua http://127.0.0.1:8080/ -- 16
```

### io_uring

`-DHTTP_WITH_IO_URING=ON` (Linux, liburing, a 6.x kernel) swaps the epoll
backend for one on io_uring: every read and write of a round goes to the
kernel in the one `io_uring_enter` that also waits for completions, reads
take their buffer from a group the loop provides, accept is multishot. Same
`IoLoop` API, same behavior, all tests pass on it. Measured against the
epoll backend on the same cores (a different day than the table above; both
backends ran faster than it shows):

| Load                                        | epoll       | io_uring    |
|---------------------------------------------|------------:|------------:|
| 1 thread, 64 conns, bombardier              |  120k req/s |  109k req/s |
| 1 thread, 64 conns, wrk pipelined x16       | 1004k req/s |  975k req/s |
| 1 thread, 1024 conns, bombardier            |  113k req/s |   89k req/s |
| 12 threads, 256 conns, bombardier           |  806k req/s |  815k req/s |
| 12 threads, 256 conns, wrk pipelined x16    | 11.1M req/s | 10.9M req/s |
| WebSocket echo, 1 thread, 64 conns, 1 KiB   |  119k msg/s |  111k msg/s |
| WebSocket echo, 1 thread, 512 conns, 16 KiB |   59k msg/s |   55k msg/s |

It halves the server's user-space CPU per request (0.5 us against 0.9 us)
but the kernel does more work per operation than the two syscalls it
replaces, so on this kernel it lands a little below epoll on one core and
level with it across threads. epoll stays the default; the backend is
there for kernels and workloads where the balance tips the other way, and
as the base for what io_uring can do that epoll cannot (registered files,
zero-copy sends). Two kernel behaviors to know about, both seen on
Ubuntu's 6.8: `IORING_REGISTER_PBUF_RING` is refused with `EINVAL` (the
backend provides buffers with `IORING_OP_PROVIDE_BUFFERS` instead), and a
thread waiting in `io_uring_enter` can make a *timed* blocking socket read
on another thread of the same process return `EINTR` once -- code doing
such reads alongside this backend must retry on `EINTR`, as the tests do.

## License

Apache License 2.0; see [LICENSE](LICENSE) and [NOTICE](NOTICE).
