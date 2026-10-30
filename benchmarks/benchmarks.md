# Benchmarking libhttp

How the numbers in the README's Performance section were produced, and how
to reproduce or extend them. Everything here runs on one Linux machine with
the server and the load generator pinned to disjoint cores.

## What is measured

`hello.cpp` (built as `http_bench_hello` with `-DHTTP_BUILD_BENCHMARKS=ON`)
is the libhttp counterpart of uWebSockets' HelloWorld:

```
http_bench_hello [threads] [port]      # defaults: hardware threads, 8080
```

| Route          | Response                                  |
|----------------|-------------------------------------------|
| `GET /`        | `200`, `Hello, World!`, `text/plain`      |
| `GET /json`    | `200`, `{"message":"Hello, World!"}`      |
| WebSocket `/`  | echoes every message (text or binary)     |

It raises `maxConnectionsPerLoop` to 4000 and logs at `Warn`, so the log is
not part of the measurement. `SIGINT`/`SIGTERM` stop it cleanly.

## Building

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHTTP_BUILD_BENCHMARKS=ON
cmake --build build -j
```

`Release` matters: the default build type is unoptimized in CMake, and the
library's own `-O2` flag applies to the library only.

## Load generators

None of these are usually installed; all build or download in a minute.

- **bombardier** (keep-alive HTTP/1.1, no pipelining; Go, static binary):
  <https://github.com/codesenberg/bombardier/releases> -- download
  `bombardier-linux-amd64`, `chmod +x`.
- **wrk** (keep-alive HTTP/1.1, scriptable): `git clone
  https://github.com/wg/wrk && make -C wrk -j`. `pipeline.lua` in this
  folder makes each connection send `N` requests per write:
  `wrk -s benchmarks/pipeline.lua URL -- N`.
- **uWebSockets' `load_test`** (WebSocket echo load): in a uWebSockets
  checkout with the uSockets submodule built (`make -C uSockets WITH_LTO=1`),
  `gcc -O3 -std=c11 -IuSockets/src -DLIBUS_NO_SSL benchmarks/load_test.c
  uSockets/uSockets.a -lz -lpthread -o ws_load_test`. Usage:
  `ws_load_test <connections> <host> <port> 0 0 [payload_bytes]`. It prints
  `Msg/sec` every four seconds and runs until killed; pipe it through
  `stdbuf -oL` when capturing to a file, or the output is lost when it is
  killed.

## Running

Pin the server to one set of cores and the client to another so they never
compete. On a two-socket machine, one socket each is the cleanest split;
`lscpu -e=CPU,CORE,NODE` shows the layout. Raise the file-descriptor limit
for connection counts in the thousands.

```sh
ulimit -n 65536
taskset -c 0-23 build/http_bench_hello 12 8080 &

# keep-alive, no pipelining
taskset -c 24-47 bombardier -c 256 -d 10s -l --fasthttp http://127.0.0.1:8080/

# 16 requests per write
taskset -c 24-47 wrk -t 12 -c 256 -d 10s --latency \
    -s benchmarks/pipeline.lua http://127.0.0.1:8080/ -- 16

# WebSocket echo, 64 connections, 1 KiB messages
taskset -c 24-47 stdbuf -oL ws_load_test 64 127.0.0.1 8080 0 0 1024
```

Two kinds of run answer different questions:

- **Per-core cost**: one server thread pinned to a single core (`taskset -c 2
  build/http_bench_hello 1 8080`) with enough connections to saturate it (64
  is plenty). The core sits at 100%, so requests per second is a direct
  measure of CPU per request. `strace -c -f` on this configuration gives
  syscalls per request, which is the most telling single number: the target
  is one `recvfrom` and one `sendto` per request and nothing else.
- **Scaling**: as many server threads as cores on the server's socket, with
  hundreds of connections. Without pipelining the load generator is the
  limit long before the server (both libhttp and uWebSockets sit around 60%
  CPU waiting for bombardier on this machine); with pipelining the server is
  the limit and the numbers reflect it.

`/proc/<pid>/stat` fields 14 and 15 (user and system ticks) before and after
a run give CPU seconds per request for the server process, which is how the
user-space share was tracked while optimizing.

## Reference: uWebSockets

For a like-for-like comparison, build uWebSockets from
<https://github.com/uNetworking/uWebSockets> (`git clone --recursive`), then:

```sh
make -C uSockets WITH_LTO=1
g++ -std=c++20 -O3 -flto -DUWS_NO_ZLIB -DLIBUS_NO_SSL -Isrc -IuSockets/src \
    hello.cpp uSockets/uSockets.a -lpthread -o uws_hello
```

where `hello.cpp` starts one `uWS::App` per thread, each calling
`.get("/*", ...)->end("Hello, World!")` and `.listen(port, ...)` -- every
App gets its own `SO_REUSEPORT` listener, matching libhttp's `ioThreads`.
For the WebSocket echo comparison, `.ws<PerSocketData>("/*", {...})` with
compression disabled and a `message` handler that calls `ws->send(message,
opCode)`.

## Results (September 2026)

2x Intel Xeon Gold 6252 (12 cores / 24 threads per socket, 2.1 GHz), Linux
6.8, glibc 2.39, GCC 13.3. Server on socket 0 (cpus 0-23), client on socket
1 (cpus 24-47). bombardier 1.2.6, wrk 4.2.0, uWebSockets master of the same
day. "Before" is the library as of HTTP-26: level-triggered epoll toggled
around every operation, a write per response head and per body, a
`getpeername` per request, a session allocated per connection.

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

Syscalls per request on a two-thread bombardier run, from `strace -c` (an
`epoll_wait` is shared by every connection ready at once, so it is not a
per-request cost):

| Syscall      | before | now  | uWebSockets |
|--------------|-------:|-----:|------------:|
| `sendto`     |  2.0   | 1.0  | 1.0         |
| `recvfrom`   |  1.3   | 1.0  | 1.0         |
| `getpeername`|  1.0   | 0    | 0           |
| `epoll_ctl`  |  0.7   | 0    | 0           |

On the saturated single core roughly 90% of the time is now spent inside
`recv`, `send` and `epoll_wait`; user-space work per request is about
1.5 us against uWebSockets' 1.1 us, which is the remaining few percent.

Run-to-run noise is around 5% for the single-thread rows and more for the
multi-thread bombardier rows, which are client-bound. Repeat runs and compare
medians before drawing conclusions from differences smaller than that.

## The io_uring backend

Build liburing (Ubuntu's `liburing-dev` works where present; otherwise
`git clone https://github.com/axboe/liburing && cd liburing && ./configure
--prefix=$PWD/install && make -C src && make install`) and configure with

```sh
cmake -S . -B build-uring -DCMAKE_BUILD_TYPE=Release -DHTTP_WITH_IO_URING=ON \
    -DCMAKE_PREFIX_PATH=/path/to/liburing/install -DHTTP_BUILD_BENCHMARKS=ON
```

The same `http_bench_hello` then runs on io_uring; nothing else changes.
Measured the same way as above, epoll and io_uring builds back to back on
the same cores (both faster than the table above; the machine was quieter):

| Load                                        | epoll       | io_uring    | server CPU/req (user) |
|---------------------------------------------|------------:|------------:|----------------------:|
| 1 thread, 64 conns, bombardier              |  120k req/s |  109k req/s | 0.89 us -> 0.51 us    |
| 1 thread, 64 conns, wrk pipelined x16       | 1004k req/s |  975k req/s | 0.35 us -> 0.29 us    |
| 1 thread, 1024 conns, bombardier            |  113k req/s |   89k req/s | 1.15 us -> 0.91 us    |
| 12 threads, 256 conns, bombardier           |  806k req/s |  815k req/s | 1.48 us -> 1.01 us    |
| 12 threads, 256 conns, wrk pipelined x16    | 11.1M req/s | 10.9M req/s | 0.38 us -> 0.35 us    |
| WebSocket echo, 1 thread, 64 conns, 1 KiB   |  119k msg/s |  111k msg/s | 2.9 us -> 1.7 us      |
| WebSocket echo, 1 thread, 512 conns, 16 KiB |   59k msg/s |   55k msg/s | 7.7 us -> 8.9 us      |

User-space time drops as expected (no per-request syscall entry, one
`io_uring_enter` per round), but system time per request rises more than
that -- io_uring's per-operation cost (request setup, poll arming, task
work, completion posting, and with the legacy provided-buffers path a
kernel allocation per selected buffer) exceeds what two direct syscalls
cost when those two are all epoll needs. With more connections than
provided buffers (the 1024-connection row; the group holds 512 x 32 KiB per
loop) reads that find no buffer are retried on the next round, which costs
throughput and tail latency. Things that could tip it: registered files
(`IOSQE_FIXED_FILE`), a kernel that accepts `IORING_REGISTER_PBUF_RING`
(no per-buffer allocation), multishot receive.

Two kernel behaviours observed on Ubuntu's 6.8.0 while doing this, both
reproduced with a bare liburing program independent of libhttp --
`benchmarks/io_uring_probe.cpp`, built as `http_io_uring_probe` alongside
the benchmark server when `HTTP_WITH_IO_URING` is on. Run it on any kernel
in question: it prints the errno of each buffer-ring registration attempt
and a count of spurious EINTRs (0 means the kernel is clean). Neither is a
Hyper-V artefact: both are argument checks and task state decided in kernel
code, and the AppArmor io_uring sysctl was 0 on the machine that showed them.

- `IORING_REGISTER_PBUF_RING` returns `EINVAL` for every registration
  (user memory or `IOU_PBUF_RING_MMAP`, any size or group); the backend
  therefore uses `IORING_OP_PROVIDE_BUFFERS`, which works.
- A thread waiting in `io_uring_enter` -- even on a ring with no operations
  at all -- makes a blocking socket read *with a receive timeout* on another
  thread of the same process return `EINTR` once, with no signal delivered.
  An in-process test client must retry on `EINTR`; `tests/test_tls_stream.cpp`
  does.
