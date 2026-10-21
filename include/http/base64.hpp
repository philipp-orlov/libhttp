// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace http {

// RFC 4648 base64 (standard alphabet, '=' padding), self-contained since no
// 3rd-party dependency is permitted. Used by the WebSocket handshake's
// Sec-WebSocket-Accept computation; also available generally for encoding
// binary data into headers/cookies and decoding it back.
std::string base64Encode(const uint8_t* data, size_t len);

// Decodes `input`; non-alphabet characters (padding, whitespace, etc.) are
// skipped rather than rejected.
std::string base64Decode(std::string_view input);

}  // namespace http
