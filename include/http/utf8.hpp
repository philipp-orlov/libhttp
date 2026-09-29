// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>

namespace http {

inline bool isValidUtf8(std::string_view text)
{
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(text.data());
    const size_t size = text.size();
    uint32_t codepoint = 0;
    uint32_t minimum = 0;
    unsigned remaining = 0;
    size_t i = 0;
    while (i < size) {
        if (!remaining) {
            // Eight bytes at a time while they are plain ASCII, which most
            // text is most of the time.
            while (i + 8 <= size) {
                uint64_t word;
                std::memcpy(&word, bytes + i, sizeof(word));
                if (word & 0x8080808080808080ull)
                    break;

                i += 8;
            }
            if (i >= size)
                break;

            const unsigned char byte = bytes[i++];
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
            const unsigned char byte = bytes[i++];
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

}  // namespace http