// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string_view>

namespace json {

inline bool isValidUtf8(std::string_view text)
{
    uint32_t codepoint = 0;
    uint32_t minimum = 0;
    unsigned remaining = 0;
    for (unsigned char byte : text) {
        if (!remaining) {
            if (byte < 0x80)
                continue;

            if (byte >= 0xC2 && byte <= 0xDF) {
                remaining = 1;
                codepoint = byte & 0x1F;
                minimum = 0x80;
            } else if (byte >= 0xE0 && byte <= 0xEF) {
                remaining = 2;
                codepoint = byte & 0x0F;
                minimum = 0x800;
            } else if (byte >= 0xF0 && byte <= 0xF4) {
                remaining = 3;
                codepoint = byte & 0x07;
                minimum = 0x10000;
            } else
                return false;
        } else {
            if ((byte & 0xC0) != 0x80)
                return false;

            codepoint = (codepoint << 6) | (byte & 0x3F);
            if (--remaining == 0 && (codepoint < minimum || codepoint > 0x10FFFF ||
                                     (codepoint >= 0xD800 && codepoint <= 0xDFFF)))
                return false;
        }
    }
    return remaining == 0;
}

}  // namespace json
