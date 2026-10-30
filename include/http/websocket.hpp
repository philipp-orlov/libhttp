// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace http {

enum class WsOpcode : uint8_t
{
    Continuation = 0x0,
    Text = 0x1,
    Binary = 0x2,
    Close = 0x8,
    Ping = 0x9,
    Pong = 0xA,
};

struct WsFrameHeader
{
    bool fin = false;
    WsOpcode opcode = WsOpcode::Continuation;
    bool masked = false;
    uint64_t payloadLen = 0;
    uint8_t maskKey[4] = {0, 0, 0, 0};
    size_t headerSize = 0;  // bytes consumed by the header itself
};

// RFC 6455 §1.3 handshake response value for a client's Sec-WebSocket-Key.
std::string computeWebSocketAccept(std::string_view clientKey);

// Parses one frame header from the front of buf. Returns false if buf
// doesn't yet contain a complete header (caller should read more and
// retry); does not consume/validate the payload itself.
bool tryParseFrameHeader(std::string_view buf, WsFrameHeader& out);

// In place, a word at a time.
void applyMask(char* data, size_t len, const uint8_t maskKey[4]);

// Server->client frames are never masked, per RFC 6455.
std::string encodeFrame(WsOpcode opcode, std::string_view payload, bool fin = true);
// The same frame written into a buffer the caller owns and reuses: `out` is
// cleared first, and once it has held a frame this size it grows no more.
void encodeFrame(WsOpcode opcode, std::string_view payload, bool fin, std::string& out);
// The frame appended after whatever `out` already holds, so several frames
// can be built into one buffer and written together.
void appendFrame(WsOpcode opcode, std::string_view payload, bool fin, std::string& out);
// Just the header of such a frame, into `out` (10 bytes suffice); returns
// its size.
size_t encodeFrameHeader(WsOpcode opcode, size_t payloadLength, bool fin, char* out);

}  // namespace http
