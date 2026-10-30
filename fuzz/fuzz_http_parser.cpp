// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// The request parser must give the same answer however the bytes are split
// across reads, and must never misbehave on any input (run under ASan/UBSan).
//
// Input: 8 bytes seeding the split pattern, then the wire bytes.

#include "http/http_parser.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace {
struct Outcome
{
    http::ParseStatus status = http::ParseStatus::NeedMore;
    int errorStatus = 0;
    size_t consumed = 0;
    std::string path;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    int method = 0;
    bool keepAlive = false;
    bool upgrade = false;

    bool operator==(const Outcome& other) const
    {
        return status == other.status && errorStatus == other.errorStatus &&
               consumed == other.consumed && path == other.path && body == other.body &&
               headers == other.headers && method == other.method &&
               keepAlive == other.keepAlive && upgrade == other.upgrade;
    }
};

Outcome describe(http::ParseStatus status, const http::HttpParser& parser,
                 const http::HttpRequest& request, size_t consumed)
{
    Outcome outcome;
    outcome.status = status;
    if (status == http::ParseStatus::Error) {
        outcome.errorStatus = parser.errorStatus();
        return outcome;
    }
    if (status != http::ParseStatus::Complete)
        return outcome;

    outcome.consumed = consumed;
    outcome.path = std::string(request.path);
    outcome.body = std::string(request.body.view());
    for (const auto& header : request.headers.items())
        outcome.headers.emplace_back(std::string(header.first), std::string(header.second));

    outcome.method = static_cast<int>(request.method);
    outcome.keepAlive = request.keepAlive;
    outcome.upgrade = request.isUpgrade;
    return outcome;
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size < 8)
        return 0;

    uint64_t seed = 0;
    for (int index = 0; index < 8; ++index)
        seed = (seed << 8) | data[index];

    const std::string wire(reinterpret_cast<const char*>(data) + 8, size - 8);
    std::mt19937_64 random(seed);

    // Whole buffer at once.
    http::HttpParser whole;
    whole.setMaxHeaderBytes(4096);
    whole.setMaxBodyBytes(1 << 20);
    http::HttpRequest wholeRequest;
    std::string error;
    size_t consumed = 0;
    const http::ParseStatus wholeStatus = whole.parse(wire, consumed, wholeRequest, error);
    const Outcome expected = describe(wholeStatus, whole, wholeRequest, consumed);

    // The same bytes in random pieces, the way the session feeds it.
    http::HttpParser pieces;
    pieces.setMaxHeaderBytes(4096);
    pieces.setMaxBodyBytes(1 << 20);
    pieces.setIncrementalConsumption(true);
    http::HttpRequest pieceRequest;
    std::string pending;
    size_t total = 0;
    http::ParseStatus status = http::ParseStatus::NeedMore;
    size_t position = 0;
    while (position < wire.size() && status == http::ParseStatus::NeedMore) {
        const size_t piece = 1 + random() % 64;
        pending.append(wire, position, piece);
        position += std::min(piece, wire.size() - position);
        size_t taken = 0;
        status = pieces.parse(pending, taken, pieceRequest, error);
        if (status == http::ParseStatus::Complete)
            total += taken;
        else
            pending.erase(0, taken);

        if (status == http::ParseStatus::NeedMore)
            total += taken;
    }
    if (status == http::ParseStatus::NeedMore && wholeStatus == http::ParseStatus::NeedMore)
        return 0;

    // Every byte has been fed: a request the one-shot parse finished (or refused) is
    // finished (or refused) in pieces too.
    if (status == http::ParseStatus::NeedMore)
        std::abort();

    // A request complete in one call is complete in pieces, at the same length.
    Outcome actual = describe(status, pieces, pieceRequest, total);
    if (wholeStatus == http::ParseStatus::Complete && status == http::ParseStatus::Complete &&
        !(actual == expected))
        std::abort();

    if (wholeStatus != http::ParseStatus::NeedMore && status != http::ParseStatus::NeedMore &&
        wholeStatus != status)
        std::abort();

    if (wholeStatus == http::ParseStatus::Error && status == http::ParseStatus::Error &&
        expected.errorStatus != actual.errorStatus)
        std::abort();

    return 0;
}

std::vector<std::string> fuzzSeeds()
{
    const std::string prefix("\x01\x02\x03\x04\x05\x06\x07\x08", 8);
    return {
        prefix + "GET /index.html?x=1&y=%20z HTTP/1.1\r\nHost: example\r\nAccept: */*\r\n\r\n",
        prefix + "POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 11\r\n\r\nhello world",
        prefix + "POST /c HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"
                 "5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\nX-T: 1\r\n\r\n",
        prefix + "GET /ws HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n"
                 "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
        prefix + "POST / HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: 3\r\n\r\nabc"
                 "GET /next HTTP/1.1\r\nHost: x\r\n\r\n",
        prefix + "GET / HTTP/1.0\r\n\r\n",
    };
}
