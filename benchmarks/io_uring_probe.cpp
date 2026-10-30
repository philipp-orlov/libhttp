// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

//
// Diagnoses two kernel behaviours the io_uring backend has to live with, so
// they can be checked on another kernel or machine in a minute. Uses
// liburing only -- no libhttp -- so a result says something about the kernel,
// not about the library.
//
//   http_io_uring_probe
//
// 1. Registered buffer rings (IORING_REGISTER_PBUF_RING): tries user-memory
//    and kernel-mmap registration with several sizes and group ids and
//    prints each errno. Ubuntu's 6.8.0-139 answers EINVAL to all of them;
//    stable 6.8 source accepts these arguments. The backend falls back to
//    IORING_OP_PROVIDE_BUFFERS, which is also checked here.
// 2. Spurious EINTR: a thread that only waits in io_uring_enter (no
//    operations at all) alongside two threads exchanging bytes over a socket
//    with plain blocking syscalls, the reading one with SO_RCVTIMEO. On
//    Ubuntu's 6.8.0-139 that read returns EINTR once per waiting ring, with
//    no signal delivered. A count of 0 across all setups means the kernel at
//    hand does not do this.
//
// Exit status is 0 whatever is found; the output is the result.

#include <liburing.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <thread>

namespace {

const char* describe(int result)
{
    return result == 0 ? "ok" : std::strerror(-result);
}

void probeBufferRings()
{
    std::puts("== registered buffer rings (IORING_REGISTER_PBUF_RING)");
    io_uring ring{};
    io_uring_params params{};
    params.flags = IORING_SETUP_CQSIZE;
    params.cq_entries = 4096;
    int rc = io_uring_queue_init_params(1024, &ring, &params);
    if (rc < 0) {
        std::printf("   io_uring_queue_init: %s -- io_uring unavailable\n", describe(rc));
        return;
    }
    std::printf("   ring features: 0x%x\n", params.features);

    // Kernel-allocated ring, mapped afterwards (IOU_PBUF_RING_MMAP, 6.4+).
    {
        io_uring_buf_reg reg{};
        reg.ring_entries = 256;
        reg.bgid = 0;
        reg.flags = IOU_PBUF_RING_MMAP;
        rc = io_uring_register_buf_ring(&ring, &reg, 0);
        std::printf("   kernel-mmap mode, 256 entries, bgid 0: %s\n", describe(rc));
        if (rc == 0)
            io_uring_unregister_buf_ring(&ring, 0);
    }
    // User memory, page-aligned, the classic form (5.19+).
    for (unsigned entries : {16u, 256u, 1024u}) {
        for (int bgid : {0, 1, 7}) {
            void* memory = mmap(nullptr, 65536, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
            io_uring_buf_reg reg{};
            reg.ring_addr = reinterpret_cast<unsigned long>(memory);
            reg.ring_entries = entries;
            reg.bgid = static_cast<unsigned short>(bgid);
            rc = io_uring_register_buf_ring(&ring, &reg, 0);
            std::printf("   user memory, %4u entries, bgid %d: %s\n", entries, bgid, describe(rc));
            if (rc == 0)
                io_uring_unregister_buf_ring(&ring, bgid);

            munmap(memory, 65536);
        }
    }
    // The fallback the backend uses.
    {
        static char buffers[16][1024];
        io_uring_sqe* sqe = io_uring_get_sqe(&ring);
        io_uring_prep_provide_buffers(sqe, buffers, 1024, 16, 3, 0);
        io_uring_submit(&ring);
        io_uring_cqe* cqe = nullptr;
        rc = io_uring_wait_cqe(&ring, &cqe);
        std::printf("   IORING_OP_PROVIDE_BUFFERS (fallback): %s\n",
                    rc < 0 ? describe(rc) : describe(cqe->res < 0 ? cqe->res : 0));
        if (rc == 0)
            io_uring_cqe_seen(&ring, cqe);
    }
    io_uring_queue_exit(&ring);
}

void spin(int milliseconds)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < until) {
    }
}

// One trial: a ring that only waits, an echo thread on plain syscalls, and
// this thread reading with a receive timeout. Returns how many reads got
// EINTR.
int eintrTrial(unsigned flags, bool armEventfdRead, bool createOnThisThread)
{
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
        return -1;

    timeval timeout{5, 0};
    setsockopt(sockets[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    std::atomic<bool> stop{false};
    io_uring ring{};
    bool ringOk = false;
    const int wakeup = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    uint64_t wakeupValue = 0;
    const auto create = [&] {
        io_uring_params params{};
        params.flags = flags | IORING_SETUP_CQSIZE;
        params.cq_entries = 16384;
        ringOk = io_uring_queue_init_params(4096, &ring, &params) == 0;
    };
    if (createOnThisThread)
        create();

    std::thread waiter([&] {
        if (!createOnThisThread)
            create();

        if (!ringOk)
            return;

        if (flags & IORING_SETUP_R_DISABLED)
            io_uring_enable_rings(&ring);

        if (armEventfdRead) {
            io_uring_sqe* sqe = io_uring_get_sqe(&ring);
            io_uring_prep_read(sqe, wakeup, &wakeupValue, sizeof(wakeupValue), 0);
            io_uring_sqe_set_data64(sqe, 1);
        }
        while (!stop.load()) {
            __kernel_timespec wait{0, 20 * 1000000};  // 20 ms
            io_uring_cqe* cqe = nullptr;
            io_uring_submit_and_wait_timeout(&ring, &cqe, 1, &wait, nullptr);
            unsigned head = 0;
            unsigned seen = 0;
            io_uring_for_each_cqe(&ring, head, cqe) { ++seen; }
            io_uring_cq_advance(&ring, seen);
        }
        io_uring_queue_exit(&ring);
    });
    std::thread echo([&] {
        char buffer[4096];
        for (;;) {
            const ssize_t count = read(sockets[0], buffer, sizeof(buffer));
            if (count <= 0)
                break;

            spin(3);  // long enough for the reader to be blocked when it happens
            if (write(sockets[0], buffer, static_cast<size_t>(count)) != count)
                break;
        }
    });

    int interruptions = 0;
    for (int i = 0; i < 60; ++i) {
        char message[293];
        std::memset(message, 'a' + i % 26, sizeof(message));
        if (write(sockets[1], message, sizeof(message)) != static_cast<ssize_t>(sizeof(message)))
            break;

        size_t got = 0;
        char incoming[293];
        while (got < sizeof(message)) {
            const ssize_t count = read(sockets[1], incoming + got, sizeof(message) - got);
            if (count < 0) {
                if (errno == EINTR) {
                    ++interruptions;
                    std::printf("      EINTR on exchange %d\n", i);
                    continue;
                }
                std::perror("      read");
                break;
            }
            if (count == 0)
                break;

            got += static_cast<size_t>(count);
        }
    }
    close(sockets[1]);
    echo.join();
    close(sockets[0]);
    stop = true;
    waiter.join();
    close(wakeup);
    return ringOk ? interruptions : -1;
}

void probeEintr()
{
    std::puts("== spurious EINTR on a timed read while another thread waits in io_uring_enter");
    struct Setup
    {
        const char* name;
        unsigned flags;
    };
    const Setup setups[] = {
        {"plain", 0},
        {"single_issuer|defer_taskrun|r_disabled",
         IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_R_DISABLED},
        {"coop_taskrun", IORING_SETUP_COOP_TASKRUN},
    };
    int total = 0;
    for (const Setup& setup : setups) {
        for (int eventfdRead : {0, 1}) {
            for (int onThisThread : {1, 0}) {
                int count = 0;
                for (int repeat = 0; repeat < 3; ++repeat) {
                    const int result = eintrTrial(setup.flags, eventfdRead != 0, onThisThread != 0);
                    if (result < 0) {
                        std::printf("   %-40s: ring setup failed, skipped\n", setup.name);
                        count = -1;
                        break;
                    }
                    count += result;
                }
                if (count >= 0) {
                    std::printf("   %-40s eventfd-read=%d ring-created-on-reader-thread=%d: %d EINTR in 3 trials\n",
                                setup.name, eventfdRead, onThisThread, count);
                    total += count;
                }
            }
        }
    }
    std::printf("   total: %d (0 means this kernel does not do it)\n", total);
}

}  // namespace

int main()
{
    probeBufferRings();
    probeEintr();
    return 0;
}
