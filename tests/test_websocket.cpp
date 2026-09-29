// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/websocket.hpp"

#include <cstdio>
#include <stdexcept>
#include <vector>

int main()
{
    if (http::computeWebSocketAccept("dGhlIHNhbXBsZSBub25jZQ==") != "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")
        return 1;

    const std::vector<std::string> invalid = {
        std::string("\xc1\x80", 2),
        std::string("\x09\x80", 2),
        std::string("\x89\xfe", 2),
        std::string("\x83\x80", 2),
        std::string("\x82\xfe\x00\x01", 4),
        std::string("\x82\xff\x80\x00\x00\x00\x00\x00\x00\x00", 10)};
    for (const auto& frame : invalid) {
        bool rejected = false;
        http::WsFrameHeader header;
        try {
            http::tryParseFrameHeader(frame, header);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        if (!rejected)
            return 2;
    }
    const auto frame = http::encodeFrame(http::WsOpcode::Binary, std::string(200, 'x'));
    for (size_t count = 0; count < 4; ++count) {
        http::WsFrameHeader header;
        if (http::tryParseFrameHeader(std::string_view(frame).substr(0, count), header))
            return 3;
    }
    http::WsFrameHeader header;
    if (!http::tryParseFrameHeader(frame, header) || header.payloadLen != 200 ||
        header.headerSize != 4)
        return 4;

    std::puts("WebSocket: RFC handshake vector, invalid headers and fragmentation passed");
}