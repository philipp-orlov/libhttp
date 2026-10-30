// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/websocket.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>

#include "http/base64.hpp"

namespace http {

namespace {
constexpr std::string_view kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// SHA-1 (RFC 3174), self-contained like base64Encode above -- RFC 6455
// mandates it for the handshake regardless of it being obsolete elsewhere,
// and this was the only thing that needed OpenSSL linked into WebSocket.
std::array<uint8_t, 20> sha1(std::string_view message)
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};

    std::string padded(message);
    const uint64_t bitLength = static_cast<uint64_t>(message.size()) * 8;
    padded.push_back(static_cast<char>(0x80));
    while (padded.size() % 64 != 56)
        padded.push_back('\0');
    for (int i = 7; i >= 0; --i)
        padded.push_back(static_cast<char>((bitLength >> (i * 8)) & 0xFF));

    auto rotl = [](uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); };

    for (size_t chunk = 0; chunk < padded.size(); chunk += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint8_t>(padded[chunk + i * 4]) << 24) |
                  (static_cast<uint8_t>(padded[chunk + i * 4 + 1]) << 16) |
                  (static_cast<uint8_t>(padded[chunk + i * 4 + 2]) << 8) |
                  static_cast<uint8_t>(padded[chunk + i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i)
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t temp = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = temp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    std::array<uint8_t, 20> digest{};
    for (int i = 0; i < 5; ++i) {
        digest[i * 4 + 0] = static_cast<uint8_t>((h[i] >> 24) & 0xFF);
        digest[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xFF);
        digest[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xFF);
        digest[i * 4 + 3] = static_cast<uint8_t>(h[i] & 0xFF);
    }
    return digest;
}
}  // namespace

std::string computeWebSocketAccept(std::string_view clientKey)
{
    std::string combined(clientKey);
    combined += kWsGuid;

    std::array<uint8_t, 20> digest = sha1(combined);
    return base64Encode(digest.data(), digest.size());
}

bool tryParseFrameHeader(std::string_view buf, WsFrameHeader& out)
{
    if (buf.size() < 2)
        return false;

    uint8_t b0 = static_cast<uint8_t>(buf[0]);
    uint8_t b1 = static_cast<uint8_t>(buf[1]);
    const uint8_t opcode = b0 & 0x0F;
    if ((b0 & 0x70) || (opcode > 2 && opcode < 8) || opcode > 10 ||
        (opcode >= 8 && (!(b0 & 0x80) || (b1 & 0x7F) > 125))) {
        throw std::invalid_argument("invalid WebSocket frame");
    }

    out.fin = (b0 & 0x80) != 0;
    out.opcode = static_cast<WsOpcode>(b0 & 0x0F);
    out.masked = (b1 & 0x80) != 0;
    uint64_t len = b1 & 0x7F;

    size_t pos = 2;
    if (len == 126) {
        if (buf.size() < pos + 2)
            return false;

        len = (static_cast<uint64_t>(static_cast<uint8_t>(buf[pos])) << 8) |
              static_cast<uint8_t>(buf[pos + 1]);
        pos += 2;
        if (len < 126)
            throw std::invalid_argument("nonminimal WebSocket length");
    } else if (len == 127) {
        if (buf.size() < pos + 8)
            return false;

        len = 0;
        for (int i = 0; i < 8; ++i) {
            len = (len << 8) | static_cast<uint8_t>(buf[pos + i]);
        }
        pos += 8;
        if (len < 65536 || (len >> 63))
            throw std::invalid_argument("invalid WebSocket length");
    }

    if (out.masked) {
        if (buf.size() < pos + 4)
            return false;

        for (int i = 0; i < 4; ++i)
            out.maskKey[i] = static_cast<uint8_t>(buf[pos + i]);

        pos += 4;
    }

    out.payloadLen = len;
    out.headerSize = pos;
    return true;
}

void applyMask(char* data, size_t len, const uint8_t maskKey[4])
{
    // The key repeated to eight bytes, XORed a word at a time -- four
    // independent words per iteration, which the compiler vectorizes;
    // memcpy keeps the byte order of data and key aligned whatever the
    // host's endianness.
    uint8_t keyBytes[8] = {maskKey[0], maskKey[1], maskKey[2], maskKey[3],
                           maskKey[0], maskKey[1], maskKey[2], maskKey[3]};
    uint64_t key = 0;
    std::memcpy(&key, keyBytes, sizeof(key));
    size_t i = 0;
    for (; i + 32 <= len; i += 32) {
        uint64_t words[4];
        std::memcpy(words, data + i, sizeof(words));
        words[0] ^= key;
        words[1] ^= key;
        words[2] ^= key;
        words[3] ^= key;
        std::memcpy(data + i, words, sizeof(words));
    }
    for (; i + 8 <= len; i += 8) {
        uint64_t word;
        std::memcpy(&word, data + i, sizeof(word));
        word ^= key;
        std::memcpy(data + i, &word, sizeof(word));
    }
    for (; i < len; ++i)
        data[i] = static_cast<char>(static_cast<uint8_t>(data[i]) ^ maskKey[i & 3]);
}

std::string encodeFrame(WsOpcode opcode, std::string_view payload, bool fin)
{
    std::string out;
    encodeFrame(opcode, payload, fin, out);
    return out;
}

void encodeFrame(WsOpcode opcode, std::string_view payload, bool fin, std::string& out)
{
    out.clear();
    appendFrame(opcode, payload, fin, out);
}

size_t encodeFrameHeader(WsOpcode opcode, size_t len, bool fin, char* header)
{
    size_t headerSize = 2;
    header[0] = static_cast<char>((fin ? 0x80 : 0x00) | (static_cast<uint8_t>(opcode) & 0x0F));
    if (len <= 125) {
        header[1] = static_cast<char>(len);
    } else if (len <= 0xFFFF) {
        header[1] = static_cast<char>(126);
        header[2] = static_cast<char>((len >> 8) & 0xFF);
        header[3] = static_cast<char>(len & 0xFF);
        headerSize = 4;
    } else {
        header[1] = static_cast<char>(127);
        for (int i = 0; i < 8; ++i)
            header[2 + i] = static_cast<char>((static_cast<uint64_t>(len) >> ((7 - i) * 8)) & 0xFF);

        headerSize = 10;
    }
    return headerSize;
}

void appendFrame(WsOpcode opcode, std::string_view payload, bool fin, std::string& out)
{
    char header[10];
    const size_t headerSize = encodeFrameHeader(opcode, payload.size(), fin, header);
    out.reserve(out.size() + headerSize + payload.size());
    out.append(header, headerSize);
    out.append(payload.data(), payload.size());
}

}  // namespace http
