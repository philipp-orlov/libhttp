// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// WebSocket framing: header parsing on arbitrary bytes, masking, and
// encode/parse round trips.

#include "http/websocket.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    std::string_view input(reinterpret_cast<const char*>(data), size);

    // Arbitrary bytes: a header parse either needs more or reports sane sizes.
    while (!input.empty()) {
        http::WsFrameHeader header;
        bool complete = false;
        try {
            complete = http::tryParseFrameHeader(input, header);
        } catch (const std::invalid_argument&) {
            break;
        }
        if (!complete)
            break;

        if (header.headerSize < 2 || header.headerSize > 14 || header.headerSize > input.size())
            std::abort();

        const size_t available = input.size() - header.headerSize;
        const size_t payload =
            static_cast<size_t>(std::min<uint64_t>(header.payloadLen, available));
        std::string copy(input.substr(header.headerSize, payload));
        http::applyMask(copy.data(), copy.size(), header.maskKey);
        http::applyMask(copy.data(), copy.size(), header.maskKey);  // masking twice restores
        if (copy != input.substr(header.headerSize, payload))
            std::abort();

        if (header.payloadLen > available)
            break;

        input.remove_prefix(header.headerSize + payload);
    }

    // Round trip of a frame built from the same bytes.
    if (size >= 1) {
        static const http::WsOpcode kOpcodes[] = {http::WsOpcode::Text, http::WsOpcode::Binary,
                                                  http::WsOpcode::Ping, http::WsOpcode::Pong};
        const http::WsOpcode opcode = kOpcodes[data[0] % 4];
        std::string payload(reinterpret_cast<const char*>(data) + 1, size - 1);
        // Control frames carry at most 125 bytes (RFC 6455 5.5); encodeFrame refuses more.
        if ((opcode == http::WsOpcode::Ping || opcode == http::WsOpcode::Pong) &&
            payload.size() > 125)
            payload.resize(125);
        const std::string frame = http::encodeFrame(opcode, payload, true);
        http::WsFrameHeader parsed;
        if (!http::tryParseFrameHeader(frame, parsed) || parsed.opcode != opcode ||
            parsed.payloadLen != payload.size() || parsed.masked ||
            frame.size() != parsed.headerSize + payload.size() ||
            frame.substr(parsed.headerSize) != payload)
            std::abort();

        std::string appended = "prefix";
        http::appendFrame(opcode, payload, true, appended);
        if (appended.substr(6) != frame)
            std::abort();
    }
    return 0;
}

std::vector<std::string> fuzzSeeds()
{
    return {
        std::string("\x81\x05hello", 7),
        std::string("\x81\x85\x01\x02\x03\x04ijmmn", 11),
        std::string("\x82\xfe\x01\x00", 4) + std::string(256, 'b'),
        std::string("\x88\x02\x03\xe8", 4),
        std::string("\x89\x00", 2),
        std::string("\x01\x03" "abc\x80\x00", 7),
        std::string("\x81\xff\x00\x00\x00\x00\x00\x01\x00\x00", 10),
    };
}
