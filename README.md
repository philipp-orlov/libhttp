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

## WebSocket routes

`Server::onWebSocket(pattern, handler, WebSocketRouteOptions)` takes per-route settings: an `accept` hook that sees
the handshake (headers, route parameters, peer address) and returns a status (`101` to upgrade, or `401`, `403`, ...
to refuse), a `userData` handed to the handler, the `subprotocols` a client must offer (the first it offers that the
route lists is echoed in `Sec-WebSocket-Protocol`), and a `maxMessageBytes` and `idleTimeoutSeconds` for this route's
connections alone. `WebSocketConnection::send(Buffer&&)` sends a pooled block without copying it: frames of 8 KiB or
more are written from the block itself (queued behind the socket if need be, in order with the copied ones) and the
block returns to its pool once written.

## WebSocket client

`http::WebSocketClient` (`http/websocket_client.hpp`) is a synchronous client for the same protocol: `connect("ws://host:port/path", options)`
(extra headers, offered sub-protocols, connect timeout, message limit), `sendBinary`/`sendText`/`ping` from any thread,
and `receive(deadline, pingEvery)`, which answers the server's pings, reassembles fragments, pings on silence and returns
an 8-aligned view of the next message. A warm connection allocates nothing per message. `wss://` is not supported yet
(`connect` throws `WebSocketError::Kind::Tls`); the client is POSIX-only for now.

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
  fragmenting the heap. A thread whose own pool is full passes buffers on to
  one bounded list shared by all threads, and a thread whose pool is empty
  takes from it, so a document built on a worker and destroyed on a reactor
  costs no allocation either. `Json::trimCache()` hands that memory back
  (this thread's and the shared list's) after an unusually large document.
- **Numbers without allocation.** Doubles are written and read with
  `std::to_chars`/`std::from_chars`, or, on a standard library without their
  floating-point overloads (libstdc++ before 11, e.g. GCC 9 on JetPack 5), with
  `snprintf`/`strtod` in the "C" locale: never through iostreams, and with a
  `.` whatever locale the process has set. The same holds for
  `FloatParameter`/`DoubleParameter`.
- **Into memory of your own.** `dump(std::string&)` appends to a string you
  recycle; `dump(char* out, size_t capacity)` writes into memory leased
  elsewhere (an `http::Buffer`) and returns the length, writing nothing when
  that exceeds `capacity`.

## Long-running servers

What a server keeps does not depend on the largest message it has carried, and
the answers that matter most can avoid the copy:

- **Bounded retention.** A connection's Session is recycled, and each of its
  buffers (request head, response body, output, stream and WebSocket buffers,
  spare header and query strings) that grew past 64 KiB is freed when the
  connection closes. `Server::stats().retainedBytes` reports what the pooled
  Sessions keep.
- **Pooled response bodies.** `HttpContext::bytes(Buffer&&, contentType)`
  answers with a body in a block of the server's pool: lease it on a worker
  with `http::Buffer body(server.bufferPool(), size)`, encode into it
  (`body.data()`, then `body.setSize(n)`; `Json::dump(char*, size_t)` writes a
  document there), and answer from the loop. The block is sent as it is,
  together with the response head in one `sendmsg` when the socket takes it,
  and goes back to the pool once written. `std::string` bodies work as before.
- **Stream chunks without a copy.** `StreamingConnection::write(std::string&&)`
  sends a chunk from the caller's string and hands back the storage of the
  previous chunk, empty, to build the next one in.
- **WebSocket back-pressure.** `ServerConfig::dropOldestWebSocketFrames`
  drops the oldest waiting Text/Binary frames of a slow client instead of
  closing the connection at `maxWebSocketQueuedFrames`/`maxWebSocketQueuedBytes`
  (off by default). `ServerConfig::validateOutgoingWebSocketText = false`
  skips the UTF-8 scan of text the server produced itself (on by default).
- **Thread-safe hand-off.** `WebSocketConnection::post()` and
  `StreamingConnection::post()` may be called from any thread, also while the
  client disconnects; they return `false` once the connection is gone.
- **A handler that answers late.** A Session whose handler has not called
  `done` stays out of the pool until it does, however long that takes; past
  twice `requestTimeoutSeconds` (at least 60 s) this is logged. A `done`
  called again later does nothing.
- **Buffer pool classes.** Blocks come in 512 B, 4 KiB, 16 KiB, 64 KiB, 96 KiB,
  192 KiB, 256 KiB, 384 KiB, 768 KiB, then powers of two from 1 MiB to 256 MiB,
  so a body of a few hundred KiB does not take a 1 MiB block.
  `BufferPool::Config::initialSlabs` is indexed by class: write
  `initialSlabs[BufferPool::classIndex(65536)]` rather than a literal index.
- **Idle loops sleep.** A loop with nothing to do waits for its next timer, or
  for work, without waking on its own.

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
test for leaks and heap growth). `http_alloc_budget` counts the server side's
`operator new` calls after warm-up and requires zero per keep-alive request, per
request with query and route parameters from a non-loopback peer, per 1 MiB
POST answered from a worker, per JSON answer of 1000 doubles, per pooled 2 MiB
answer, per WebSocket echo and per stream chunk, and bounds what Sessions keep
after serving 20 MB responses. `http_float_fallback` runs the number tests on
the `snprintf`/`strtod` path that GCC 9 builds take.
The memory-pool regression performs over ten million block acquire/return pairs
across 64 slabs, checks their contents after shuffled reuse, and requires zero heap or
backing allocations and frees after provisioning.
Buffer tests also cover self-appends across a size-class change: the source
view must follow the data into the new block before the old block is reused.

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
  Idle slab blocks carry their occupancy index inside the unused payload, so
  acquisition validates ownership in constant time without scanning slabs or
  allocating separate per-block metadata. Returns still validate the caller's
  pointer before linking it back into the pool.
- **One receive buffer per loop.** Reads land in a 256 KiB buffer shared by
  every connection of the loop (each read is consumed before the next can
  complete) and are parsed in place; only a partial request or frame is
  copied to the connection. A WebSocket frame is unmasked in place and handed
  to the handler as a view; a large one is sent straight from that buffer
  with one `sendmsg`, copied only if the socket did not take all of it.
- **Coarse timers.** The idle timer is refreshed on every read and write but
  touches the timer heap only when it would move by more than a second. A loop
  with no timer due sleeps until the next one, with no periodic wakeup.
- **Large bodies.** A response body over 64 KiB is written from where it is
  (the handler's string or pooled block) rather than copied into the output,
  in one `sendmsg` together with the head; a request body is read straight
  into its block, as much per `recv` as the block has room for.

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
