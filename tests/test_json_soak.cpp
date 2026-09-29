// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Long-run behaviour of the JSON value.
//
// A service built on this library answers the same shaped request for months,
// so "does not grow" and "does not churn" are properties worth asserting
// rather than assuming. Three things are measured, and they fail differently:
//
//   live bytes   allocated and not yet freed. Growth here is a leak, and the
//                counters come from a replaced operator new, so it is exact.
//   heap bytes   what the allocator holds from the OS. This can climb while
//                live bytes stay flat -- that is what fragmentation looks like.
//   allocations  how many trips to malloc one request/response round costs.
//                The per-thread buffer pool is supposed to cut this down, so
//                the same round is timed with the pool warm and with it
//                dropped before every round.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

#include "json/json.hpp"

namespace {

std::atomic<long long> gLiveBytes{0};
std::atomic<long long> gLiveBlocks{0};
std::atomic<long long> gAllocations{0};

// Every block carries its size just ahead of the pointer handed out so the
// plain (unsized) delete can still account for it. One max_align_t keeps the
// payload as aligned as malloc would have.
constexpr size_t kHeader = alignof(std::max_align_t);

void* tracked(size_t size)
{
    auto* raw = static_cast<char*>(std::malloc(size + kHeader));
    if (!raw)
        throw std::bad_alloc();

    std::memcpy(raw, &size, sizeof size);
    gLiveBytes.fetch_add(static_cast<long long>(size), std::memory_order_relaxed);
    gLiveBlocks.fetch_add(1, std::memory_order_relaxed);
    gAllocations.fetch_add(1, std::memory_order_relaxed);
    return raw + kHeader;
}

void untracked(void* pointer) noexcept
{
    if (!pointer)
        return;

    char* raw = static_cast<char*>(pointer) - kHeader;
    size_t size = 0;
    std::memcpy(&size, raw, sizeof size);
    gLiveBytes.fetch_sub(static_cast<long long>(size), std::memory_order_relaxed);
    gLiveBlocks.fetch_sub(1, std::memory_order_relaxed);
    std::free(raw);
}

}  // namespace

// Every plain form has to be replaced together: the standard lets a block from
// the nothrow new come back through the sized delete, so replacing only some of
// them hands a header-carrying pointer to a deallocator that knows nothing
// about it. The over-aligned forms are left alone, which is safe because they
// only ever pair with each other.
void* operator new(size_t size)
{
    return tracked(size ? size : 1);
}
void* operator new[](size_t size)
{
    return tracked(size ? size : 1);
}
void* operator new(size_t size, const std::nothrow_t&) noexcept
{
    try {
        return tracked(size ? size : 1);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](size_t size, const std::nothrow_t&) noexcept
{
    try {
        return tracked(size ? size : 1);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* p) noexcept
{
    untracked(p);
}
void operator delete[](void* p) noexcept
{
    untracked(p);
}
void operator delete(void* p, size_t) noexcept
{
    untracked(p);
}
void operator delete[](void* p, size_t) noexcept
{
    untracked(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept
{
    untracked(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept
{
    untracked(p);
}

namespace {

using json::Json;

// Under a sanitizer the allocator is not the one being asserted about, and
// every round costs orders of magnitude more, so the workload shrinks and the
// heap plateau is not checked.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif

constexpr int kRounds = kSanitized ? 500 : 20000;
constexpr int kWarmup = 64;

size_t heapBytes()
{
#if defined(__GLIBC__) && defined(__GLIBC_MINOR__) && (__GLIBC__ > 2 || __GLIBC_MINOR__ >= 33)
    const struct mallinfo2 info = mallinfo2();
    return info.arena + info.hblkhd;
#else
    return 0;
#endif
}

// The shape an inference endpoint answers with: a handful of detections, each
// carrying per-character arrays.
Json buildResponse(int seed)
{
    Json detections = Json::array();
    for (int i = 0; i < 8; ++i) {
        Json rectangles = Json::array();
        Json confidences = Json::array();
        for (int c = 0; c < 7; ++c) {
            rectangles.push_back(Json{{"x", c * 3 + seed % 5}, {"y", 12}, {"w", 20}, {"h", 40}});
            confidences.push_back(0.5 + c / 128.0);
        }
        detections.push_back(Json{{"text", "7ABC123"},
                                  {"country", "US"},
                                  {"state", "CA"},
                                  {"confidence", 0.9731},
                                  {"countryStateConfidence", 0.881},
                                  {"characterRectangles", std::move(rectangles)},
                                  {"characterConfidences", std::move(confidences)},
                                  {"flags", Json::array()}});
    }
    return Json{
        {"status", "ok"}, {"elapsedUs", 1000 + seed}, {"detections", std::move(detections)}};
}

// One request/response round: build the document, render it into a buffer the
// caller recycles, and read it back the way a client would.
bool round(int seed, std::string& out)
{
    const Json response = buildResponse(seed);
    const size_t expected = response.measure();
    out.clear();
    response.dump(out);
    if (out.size() != expected)
        return false;

    Json parsed;
    std::string error;
    if (!Json::parse(out, parsed, error))
        return false;

    const Json* detections = parsed.find("detections");
    return detections && detections->isArray() && detections->asArray().size() == 8;
}

// Allocations one round costs. `cold` drops the pool before each one, which is
// the same work without any buffer reuse.
long long allocationsPerRound(std::string& out, bool cold)
{
    for (int i = 0; i < kWarmup; ++i) {
        if (cold)
            Json::trimCache();

        round(i, out);
    }
    const long long before = gAllocations.load();
    for (int i = 0; i < kRounds; ++i) {
        if (cold)
            Json::trimCache();

        if (!round(i % kWarmup, out))
            return -1;
    }
    return (gAllocations.load() - before) / kRounds;
}

}  // namespace

int main()
{
    std::string out;
    for (int i = 0; i < kWarmup; ++i)
        if (!round(i, out))
            return 1;

    // The round-trip has to be exact, or the rest measures the wrong thing.
    Json parsed;
    std::string error;
    if (!Json::parse(out, parsed, error) || parsed.dump() != out)
        return 2;

    Json::trimCache();
    const long long baseBytes = gLiveBytes.load();
    const long long baseBlocks = gLiveBlocks.load();
    const size_t baseHeap = heapBytes();

    const long long warm = allocationsPerRound(out, false);
    const size_t warmHeap = heapBytes();
    const long long cold = allocationsPerRound(out, true);
    if (warm < 0 || cold < 0)
        return 3;

    Json::trimCache();
    const long long leakedBytes = gLiveBytes.load() - baseBytes;
    const long long leakedBlocks = gLiveBlocks.load() - baseBlocks;
    if (leakedBytes != 0 || leakedBlocks != 0) {
        std::printf("JSON soak: leaked %lld bytes in %lld blocks\n", leakedBytes, leakedBlocks);
        return 4;
    }
    if (warm >= cold) {
        std::printf("JSON soak: pooling saved nothing (%lld vs %lld allocations/round)\n", warm,
                    cold);
        return 5;
    }
    if (!kSanitized && baseHeap && warmHeap > baseHeap) {
        std::printf("JSON soak: heap grew %zu -> %zu bytes over %d rounds\n", baseHeap, warmHeap,
                    kRounds);
        return 6;
    }

    std::printf(
        "JSON: %d rounds, %lld allocations/round pooled vs %lld unpooled, +0 bytes, heap flat at "
        "%zu\n",
        kRounds, warm, cold, warmHeap);
}
