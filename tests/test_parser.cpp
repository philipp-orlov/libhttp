// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/http_parser.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

int main()
{
    const std::vector<std::string> invalid = {
        "GET / HTTP/1.1\r\nX-Test: " + std::string(128, 'a') + "\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 7x\r\n\r\n1234567",
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 184467440737095516160\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx",
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\nx",
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabcXX0\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n10000000000000000\r\n",
        "GET / HTTP/1.1extra\r\nHost: x\r\n\r\n",
        "GET / HTTP/1.1\r\nHost : x\r\n\r\n",
        "GET /%00 HTTP/1.1\r\nHost: x\r\n\r\n"};
    for (size_t index = 0; index < invalid.size(); ++index) {
        http::HttpParser parser;
        if (index == 0)
            parser.setMaxHeaderBytes(32);

        http::HttpRequest request;
        std::string error;
        size_t consumed = 0;
        if (parser.parse(invalid[index], consumed, request, error) != http::ParseStatus::Error)
            return 1;
    }
    const std::vector<std::string> valid = {
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\n\r\n1234567",
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: "
        "ChUnKeD\r\n\r\n3\r\n123\r\n4\r\n4567\r\n0\r\nX-Test: ok\r\n\r\n"};
    for (const auto& wire : valid) {
        for (size_t split = 1; split <= wire.size(); ++split) {
            http::HttpParser parser;
            parser.setIncrementalConsumption(true);
            http::HttpRequest request;
            std::string pending;
            std::string error;
            http::ParseStatus status = http::ParseStatus::NeedMore;
            for (size_t position = 0; position < wire.size(); position += split) {
                pending.append(wire.substr(position, split));
                size_t consumed = 0;
                status = parser.parse(pending, consumed, request, error);
                if (status == http::ParseStatus::Error)
                    return 2;

                pending.erase(0, consumed);
            }
            if (status != http::ParseStatus::Complete || request.body.view() != "1234567" ||
                !pending.empty())
                return 3;
        }
    }
    http::HttpParser direct;
    direct.setIncrementalConsumption(true);
    http::HttpRequest request;
    std::string error;
    size_t consumed = 0;
    if (direct.parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\n\r\n12", consumed, request,
                     error) != http::ParseStatus::NeedMore ||
        direct.writableBodyBytes(request) != 5)
        return 4;
    std::memcpy(request.body.data() + request.body.size(), "34567", 5);
    direct.commitBodyBytes(5, request);
    if (direct.parse({}, consumed, request, error) != http::ParseStatus::Complete ||
        request.body.view() != "1234567")
        return 5;

    // A declared large body reserves nothing big until data has arrived.
    {
        http::HttpParser big;
        big.setIncrementalConsumption(true);
        http::HttpRequest bigRequest;
        std::string bigError;
        size_t bigConsumed = 0;
        std::string head = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 2000000\r\n\r\n";
        if (big.parse(head, bigConsumed, bigRequest, bigError) != http::ParseStatus::NeedMore ||
            bigRequest.body.capacity() > 16 * 1024)
            return 6;

        std::string first(20000, 'a');
        if (big.parse(first, bigConsumed, bigRequest, bigError) != http::ParseStatus::NeedMore ||
            bigRequest.body.capacity() < 2000000 || big.writableBodyBytes(bigRequest) != 1980000)
            return 7;
    }
    // Oversized declarations are answered 413.
    {
        http::HttpParser limited;
        limited.setMaxBodyBytes(1000);
        http::HttpRequest limitedRequest;
        std::string limitedError;
        size_t limitedConsumed = 0;
        if (limited.parse("POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1001\r\n\r\n", limitedConsumed,
                          limitedRequest, limitedError) != http::ParseStatus::Error ||
            limited.errorStatus() != 413)
            return 8;
    }
    // Status mapping for the limit and version errors.
    {
        struct Mapped
        {
            std::string wire;
            int status;
        };
        for (const Mapped& mapped :
             {Mapped{"GET / HTTP/2.0\r\nHost: x\r\n\r\n", 505},
              Mapped{"GET /" + std::string(80, 'a') + " HTTP/1.1\r\nHost: x\r\n\r\n", 414},
              Mapped{"GET / HTTP/1.1\r\nHost: x\r\nX: " + std::string(80, 'a') + "\r\n\r\n", 431},
              Mapped{"GET / HTTP/1.1\r\nHost x\r\n\r\n", 400}}) {
            http::HttpParser limited;
            limited.setMaxHeaderBytes(64);
            http::HttpRequest mappedRequest;
            std::string mappedError;
            size_t mappedConsumed = 0;
            if (limited.parse(mapped.wire, mappedConsumed, mappedRequest, mappedError) !=
                    http::ParseStatus::Error ||
                limited.errorStatus() != mapped.status)
                return 9;
        }
    }
    // A header line dribbled in one byte at a time is scanned once per byte, not once per read
    // (the line end may also be split across reads).
    {
        http::HttpParser dribble;
        dribble.setIncrementalConsumption(true);
        dribble.setMaxHeaderBytes(128 * 1024);
        http::HttpRequest dribbleRequest;
        std::string dribbleError;
        std::string wire = "GET / HTTP/1.1\r\nHost: x\r\nX-Long: " + std::string(100000, 'v') +
                           "\r\nX-Next: 1\r\n\r\n";
        std::string pending;
        http::ParseStatus dribbleStatus = http::ParseStatus::NeedMore;
        const auto begun = std::chrono::steady_clock::now();
        for (const char byte : wire) {
            pending.push_back(byte);
            size_t taken = 0;
            dribbleStatus = dribble.parse(pending, taken, dribbleRequest, dribbleError);
            if (dribbleStatus == http::ParseStatus::Error)
                return 10;

            pending.erase(0, taken);
        }
        const auto tookMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - begun)
                                .count();
        if (dribbleStatus != http::ParseStatus::Complete ||
            dribbleRequest.headers.get("X-Next") != "1" ||
            dribbleRequest.headers.get("X-Long").size() != 100000 || tookMs > 1500)
            return 11;

        std::printf("parser: 100 KB header line dribbled byte by byte in %lld ms\n",
                    static_cast<long long>(tookMs));
    }

    std::puts("parser: malformed framing, fragmentation boundaries and direct body receive passed");
}