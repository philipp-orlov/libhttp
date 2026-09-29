// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/http_parser.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <new>

namespace http {

namespace {
// A body's block is reserved in full only after this much of it has arrived.
constexpr size_t kBodyAdmitBytes = 16 * 1024;

// The status a parse error is answered with; anything not listed is a plain 400.
int statusForError(std::string_view error)
{
    if (error == "request line too long")
        return 414;

    if (error == "headers too long" || error == "too many headers" || error == "trailer too long")
        return 431;

    if (error == "unsupported HTTP version")
        return 505;

    if (error == "payload too large")
        return 413;

    return 400;
}
bool parseHex(std::string_view s, size_t& out)
{
    if (s.empty())
        return false;

    size_t v = 0;
    for (char c : s) {
        if (v > std::numeric_limits<size_t>::max() / 16)
            return false;

        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= static_cast<size_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= static_cast<size_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= static_cast<size_t>(c - 'A' + 10);
        else
            return false;
    }
    out = v;
    return true;
}

int hexDigit(char h)
{
    if (h >= '0' && h <= '9')
        return h - '0';

    if (h >= 'a' && h <= 'f')
        return h - 'a' + 10;

    if (h >= 'A' && h <= 'F')
        return h - 'A' + 10;

    return -1;
}

// RFC 9110 tchar, as a table: the parser runs this over every byte of every
// method and header name.
struct TokenTable
{
    bool table[256] = {};
    constexpr TokenTable()
    {
        for (int c = '0'; c <= '9'; ++c)
            table[c] = true;
        for (int c = 'A'; c <= 'Z'; ++c)
            table[c] = true;
        for (int c = 'a'; c <= 'z'; ++c)
            table[c] = true;
        for (char c : {'!', '#', '$', '%', '&', '\'', '*', '+', '-', '.', '^', '_', '`', '|', '~'})
            table[static_cast<unsigned char>(c)] = true;
    }
};
constexpr TokenTable kTokenTable{};

inline bool tokenCharacter(unsigned char value)
{
    return kTokenTable.table[value];
}

// std::all_of with a function pointer is an indirect call per byte; this
// inlines to a table lookup.
inline bool allTokenCharacters(std::string_view s)
{
    for (const char c : s) {
        if (!kTokenTable.table[static_cast<unsigned char>(c)])
            return false;
    }
    return true;
}

bool hasToken(std::string_view value, std::string_view token)
{
    while (!value.empty()) {
        size_t comma = value.find(',');
        std::string_view part = value.substr(0, comma);
        while (!part.empty() && (part.front() == ' ' || part.front() == '\t'))
            part.remove_prefix(1);
        while (!part.empty() && (part.back() == ' ' || part.back() == '\t'))
            part.remove_suffix(1);
        if (equalsIgnoreCase(part, token))
            return true;

        if (comma == std::string_view::npos)
            break;

        value.remove_prefix(comma + 1);
    }
    return false;
}
}  // namespace

std::string urlDecode(std::string_view in, bool plusAsSpace)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '%' && i + 2 < in.size()) {
            int hi = hexDigit(in[i + 1]);
            int lo = hexDigit(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
            out.push_back(c);
        } else if (plusAsSpace && c == '+') {
            out.push_back(' ');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

void HttpParser::reset()
{
    state_ = State::RequestLine;
    errorStatus_ = 400;
    scanPos_ = 0;
    contentLength_ = 0;
    chunked_ = false;
    expectContinue_ = false;
    chunkRemaining_ = 0;
    headerBytes_ = headerCount_ = trailerBytes_ = trailerCount_ = 0;
    hasContentLength_ = hasTransferEncoding_ = hasHost_ = false;
    connectionIndex_ = upgradeIndex_ = expectIndex_ = kNoHeader;
    base_ = nullptr;
    targetSpan_ = pathSpan_ = querySpan_ = Span{};
    hasQuery_ = pathEscaped_ = false;
    lineScanned_ = 0;
    headerSpans_.clear();
}

// Position of the CRLF ending the line at the start of `rest`. A line that is
// still incomplete is remembered up to where it was searched, so a client
// dribbling one byte per read costs a scan of each byte once, not once per read.
size_t HttpParser::findLineEnd(std::string_view rest)
{
    const size_t found = rest.find("\r\n", std::min(lineScanned_, rest.size()));
    if (found == std::string_view::npos) {
        // Keep the last byte in play: it may be the CR of a CRLF split across reads.
        lineScanned_ = rest.empty() ? 0 : rest.size() - 1;
        return found;
    }
    lineScanned_ = 0;
    return found;
}

ParseStatus HttpParser::needMore(size_t& consumed)
{
    // Head bytes are never given up before the head is complete: the
    // request keeps a copy of the whole head, and until then only offsets
    // into the caller's buffer have been noted. Body bytes already appended
    // to the request are.
    const bool inHead = state_ == State::RequestLine || state_ == State::Headers;
    consumed = incrementalConsumption_ && !inHead ? scanPos_ : 0;
    if (consumed)
        scanPos_ = 0;

    return ParseStatus::NeedMore;
}

bool HttpParser::parseRequestLine(std::string_view line, HttpRequest& out, std::string& error)
{
    size_t sp1 = line.find(' ');
    if (sp1 == std::string_view::npos) {
        error = "malformed request line";
        return false;
    }
    size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) {
        error = "malformed request line";
        return false;
    }

    std::string_view methodStr = line.substr(0, sp1);
    std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string_view version = line.substr(sp2 + 1);

    if (methodStr.empty() || !allTokenCharacters(methodStr) ||
        target.empty()) {
        error = "invalid request target or method";
        return false;
    }
    // One pass over the target: no controls or space, and every '%' a
    // well-formed escape other than %00.
    for (size_t index = 0; index < target.size(); ++index) {
        const unsigned char value = static_cast<unsigned char>(target[index]);
        if (value <= 32 || value == 127) {
            error = "invalid request target or method";
            return false;
        }
        if (value == '%') {
            if (target.size() - index < 3) {
                error = "invalid URL escape";
                return false;
            }
            const int hi = hexDigit(target[index + 1]);
            const int lo = hexDigit(target[index + 2]);
            if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) {
                error = "invalid URL escape";
                return false;
            }
            index += 2;
        }
    }

    out.method = parseMethod(methodStr);
    targetSpan_ = spanOf(target);

    size_t q = target.find('?');
    std::string_view rawPath = (q == std::string_view::npos) ? target : target.substr(0, q);
    hasQuery_ = q != std::string_view::npos;
    if (hasQuery_)
        querySpan_ = spanOf(target.substr(q + 1));

    pathSpan_ = spanOf(rawPath);
    pathEscaped_ = rawPath.find('%') != std::string_view::npos;

    if (version == "HTTP/1.0" || version == "HTTP/1.1") {
        out.versionMajor = version[5] - '0';
        out.versionMinor = version[7] - '0';
    } else {
        error = "unsupported HTTP version";
        return false;
    }

    out.keepAlive = !(out.versionMajor == 1 && out.versionMinor == 0);
    return true;
}

bool HttpParser::parseHeaderLine(std::string_view line, HttpRequest&, std::string& error)
{
    size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
        error = "malformed header line";
        return false;
    }
    std::string_view name = line.substr(0, colon);
    std::string_view value = line.substr(colon + 1);
    if (name.empty() || !allTokenCharacters(name)) {
        error = "invalid header";
        return false;
    }
    for (const char c : value) {
        const unsigned char v = static_cast<unsigned char>(c);
        if ((v < 32 && v != '\t') || v == 127) {
            error = "invalid header";
            return false;
        }
    }
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
        value.remove_suffix(1);

    // Only names of a matching length are compared at all.
    const size_t index = headerSpans_.size();
    switch (name.size()) {
        case 4:
            if (equalsIgnoreCase(name, "Host")) {
                if (hasHost_ || value.empty()) {
                    error = "invalid Host";
                    return false;
                }
                hasHost_ = true;
            }
            break;
        case 6:
            if (expectIndex_ == kNoHeader && equalsIgnoreCase(name, "Expect"))
                expectIndex_ = index;

            break;
        case 7:
            if (upgradeIndex_ == kNoHeader && equalsIgnoreCase(name, "Upgrade"))
                upgradeIndex_ = index;

            break;
        case 10:
            if (connectionIndex_ == kNoHeader && equalsIgnoreCase(name, "Connection"))
                connectionIndex_ = index;

            break;
        case 14:
            if (equalsIgnoreCase(name, "Content-Length")) {
                if (hasContentLength_ || hasTransferEncoding_ || value.empty()) {
                    error = "ambiguous Content-Length";
                    return false;
                }
                size_t length = 0;
                for (char digit : value) {
                    if (digit < '0' || digit > '9' ||
                        length > (std::numeric_limits<size_t>::max() -
                                  static_cast<size_t>(digit - '0')) /
                                     10) {
                        error = "invalid Content-Length";
                        return false;
                    }
                    length = length * 10 + static_cast<size_t>(digit - '0');
                }
                hasContentLength_ = true;
                contentLength_ = length;
            }
            break;
        case 17:
            if (equalsIgnoreCase(name, "Transfer-Encoding")) {
                if (hasTransferEncoding_ || hasContentLength_ || !equalsIgnoreCase(value, "chunked")) {
                    error = "unsupported or ambiguous Transfer-Encoding";
                    return false;
                }
                hasTransferEncoding_ = chunked_ = true;
            }
            break;
        default: break;
    }
    if (++headerCount_ > 100) {
        error = "too many headers";
        return false;
    }
    headerSpans_.push_back(HeaderSpan{spanOf(name), spanOf(value)});
    return true;
}

// The whole head is in `buf` (from its start up to scanPos_): copy it into
// the request once and point every view at the copy. The one decode a path
// may need happens here too, into storage of its own.
void HttpParser::materializeHead(std::string_view buf, HttpRequest& out)
{
    out.head.assign(buf.data(), scanPos_);
    const auto view = [&out](Span span) {
        return std::string_view(out.head.data() + span.offset, span.length);
    };
    out.rawTarget = view(targetSpan_);
    out.queryString = hasQuery_ ? view(querySpan_) : std::string_view{};
    if (pathEscaped_) {
        out.decodedPath = urlDecode(view(pathSpan_), /*plusAsSpace=*/false);
        out.path = out.decodedPath;
    } else {
        out.path = view(pathSpan_);
    }
    out.headers.clear();
    for (const HeaderSpan& header : headerSpans_)
        out.headers.add(view(header.name), view(header.value));
}

bool HttpParser::finalizeHeaders(HttpRequest& out, std::string& error)
{
    if (out.versionMinor == 1 && !hasHost_) {
        error = "missing Host";
        return false;
    }
    if (out.versionMinor == 0 && chunked_) {
        error = "HTTP/1.0 cannot use chunked framing";
        return false;
    }
    const auto& items = out.headers.items();
    const auto headerAt = [&items](size_t index) -> std::string_view {
        return index == kNoHeader ? std::string_view{} : std::string_view(items[index].second);
    };
    // Only meaningful with a body still to come; a request whose body is
    // already in the buffer completes before anyone asks.
    expectContinue_ = out.versionMinor == 1 && (chunked_ || contentLength_ > 0) &&
                      expectIndex_ != kNoHeader && hasToken(headerAt(expectIndex_), "100-continue");

    const std::string_view connection = headerAt(connectionIndex_);
    if (!connection.empty()) {
        if (hasToken(connection, "close"))
            out.keepAlive = false;
        else if (hasToken(connection, "keep-alive"))
            out.keepAlive = true;

        if (upgradeIndex_ != kNoHeader && hasToken(connection, "upgrade") &&
            equalsIgnoreCase(headerAt(upgradeIndex_), "websocket")) {
            out.isUpgrade = true;
            out.upgradeTo = "websocket";
            out.keepAlive = true;
        }
    }
    return true;
}

ParseStatus HttpParser::parse(std::string_view buf, size_t& consumed, HttpRequest& out,
                              std::string& error)
{
    try {
        const ParseStatus status = parseImpl(buf, consumed, out, error);
        if (status == ParseStatus::Error && errorStatus_ == 400)
            errorStatus_ = statusForError(error);

        return status;
    } catch (const std::bad_alloc&) {
        // The buffer pool has no block to give: say so instead of dropping the connection.
        error = "server busy";
        errorStatus_ = 503;
        return ParseStatus::Error;
    }
}

ParseStatus HttpParser::parseImpl(std::string_view buf, size_t& consumed, HttpRequest& out,
                                  std::string& error)
{
    consumed = 0;
    base_ = buf.data();
    for (;;) {
        switch (state_) {
            case State::RequestLine: {
                std::string_view rest = buf.substr(scanPos_);
                size_t rel = findLineEnd(rest);
                if (rel == std::string_view::npos) {
                    if (rest.size() > maxHeaderBytes_ - headerBytes_) {
                        error = "request line too long";
                        return ParseStatus::Error;
                    }
                    return needMore(consumed);
                }
                if (rel + 2 > maxHeaderBytes_ - headerBytes_) {
                    error = "request line too long";
                    return ParseStatus::Error;
                }
                headerBytes_ += rel + 2;
                if (!parseRequestLine(rest.substr(0, rel), out, error))
                    return ParseStatus::Error;

                scanPos_ += rel + 2;
                state_ = State::Headers;
                break;
            }
            case State::Headers: {
                std::string_view rest = buf.substr(scanPos_);
                size_t rel = findLineEnd(rest);
                if (rel == std::string_view::npos) {
                    if (rest.size() > maxHeaderBytes_ - headerBytes_) {
                        error = "headers too long";
                        return ParseStatus::Error;
                    }
                    return needMore(consumed);
                }
                if (rel + 2 > maxHeaderBytes_ - headerBytes_) {
                    error = "headers too long";
                    return ParseStatus::Error;
                }
                headerBytes_ += rel + 2;
                if (rel == 0) {
                    scanPos_ += 2;
                    materializeHead(buf, out);
                    if (!finalizeHeaders(out, error))
                        return ParseStatus::Error;

                    if (contentLength_ > maxBodyBytes_) {
                        error = "payload too large";
                        errorStatus_ = 413;
                        return ParseStatus::Error;
                    }
                    if (chunked_) {
                        state_ = State::ChunkSize;
                    } else if (contentLength_ > 0) {
                        // Only a small block now: the declaration of a large
                        // body costs the pool nothing until the client has
                        // sent some of it (see State::BodyLength).
                        out.body.reserve(std::min(contentLength_, kBodyAdmitBytes));
                        state_ = State::BodyLength;
                    } else {
                        state_ = State::Done;
                    }
                } else {
                    if (!parseHeaderLine(rest.substr(0, rel), out, error))
                        return ParseStatus::Error;

                    scanPos_ += rel + 2;
                }
                break;
            }
            case State::BodyLength: {
                size_t available = buf.size() - scanPos_;
                size_t need = contentLength_ - out.body.size();
                size_t take = std::min(available, need);
                if (take > 0) {
                    out.body.append(buf.data() + scanPos_, take);
                    scanPos_ += take;
                }
                if (out.body.size() >= contentLength_) {
                    state_ = State::Done;
                    break;
                }
                // The client has sent enough to be taken seriously: take the
                // block for the rest in one piece (one class, no regrowth).
                if (out.body.size() >= kBodyAdmitBytes)
                    out.body.reserve(contentLength_);

                return needMore(consumed);
            }
            case State::ChunkSize: {
                std::string_view rest = buf.substr(scanPos_);
                size_t rel = rest.find("\r\n");
                if (rel == std::string_view::npos) {
                    if (rest.size() > 64) {
                        error = "invalid chunk size line";
                        return ParseStatus::Error;
                    }
                    return needMore(consumed);
                }
                if (rel > 64) {
                    error = "invalid chunk size line";
                    return ParseStatus::Error;
                }
                std::string_view line = rest.substr(0, rel);
                size_t semi = line.find(';');
                std::string_view sizeStr =
                    (semi == std::string_view::npos) ? line : line.substr(0, semi);
                size_t chunkSize = 0;
                if (!parseHex(sizeStr, chunkSize)) {
                    error = "bad chunk size";
                    return ParseStatus::Error;
                }
                scanPos_ += rel + 2;
                chunkRemaining_ = chunkSize;
                if (chunkSize == 0) {
                    state_ = State::ChunkTrailer;
                } else {
                    if (chunkSize > maxBodyBytes_ - out.body.size()) {
                        error = "payload too large";
                        errorStatus_ = 413;
                        return ParseStatus::Error;
                    }
                    state_ = State::ChunkData;
                }
                break;
            }
            case State::ChunkData: {
                size_t available = buf.size() - scanPos_;
                size_t take = std::min(available, chunkRemaining_);
                if (take > 0) {
                    out.body.append(buf.data() + scanPos_, take);
                    scanPos_ += take;
                    chunkRemaining_ -= take;
                }
                if (chunkRemaining_ > 0)
                    return needMore(consumed);

                state_ = State::ChunkCRLF;
                break;
            }
            case State::ChunkCRLF: {
                if (buf.size() - scanPos_ < 2)
                    return needMore(consumed);

                if (buf.substr(scanPos_, 2) != "\r\n") {
                    error = "invalid chunk terminator";
                    return ParseStatus::Error;
                }
                scanPos_ += 2;  // trailing CRLF after chunk data
                state_ = State::ChunkSize;
                break;
            }
            case State::ChunkTrailer: {
                std::string_view rest = buf.substr(scanPos_);
                size_t rel = findLineEnd(rest);
                if (rel == std::string_view::npos) {
                    if (rest.size() > maxHeaderBytes_ - trailerBytes_) {
                        error = "trailer too long";
                        return ParseStatus::Error;
                    }
                    return needMore(consumed);
                }
                if (rel + 2 > maxHeaderBytes_ - trailerBytes_ || ++trailerCount_ > 100) {
                    error = "trailer too long";
                    return ParseStatus::Error;
                }
                trailerBytes_ += rel + 2;
                if (rel != 0) {
                    auto line = rest.substr(0, rel);
                    size_t colon = line.find(':');
                    auto name = line.substr(0, colon);
                    if (colon == std::string_view::npos || name.empty() ||
                        !allTokenCharacters(name) ||
                        equalsIgnoreCase(name, "Content-Length") ||
                        equalsIgnoreCase(name, "Transfer-Encoding") ||
                        std::any_of(line.begin(), line.end(), [](unsigned char value) {
                            return (value < 32 && value != '\t') || value == 127;
                        })) {
                        error = "invalid trailer";
                        return ParseStatus::Error;
                    }
                }
                scanPos_ += rel + 2;
                if (rel == 0)
                    state_ = State::Done;

                break;
            }
            case State::Done: {
                consumed = scanPos_;
                return ParseStatus::Complete;
            }
        }
    }
}

}  // namespace http
