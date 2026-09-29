// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "http/http_types.hpp"

namespace http {

enum class ParseStatus
{
    NeedMore,
    Complete,
    Error
};

// Incremental HTTP/1.1 request parser (hand-rolled -- no 3rd-party parser
// is permitted). Supports Content-Length and chunked request bodies,
// keep-alive detection, and Upgrade (for WebSocket). One instance is
// reused per-connection across requests via reset().
class HttpParser
{
public:
    void reset();

    // Parses as much of `buf` as forms a complete request. On Complete,
    // `consumed` is the number of bytes belonging to this request; bytes
    // after that in `buf` (if any) are the start of a pipelined next
    // request and must be preserved by the caller. On NeedMore, `consumed`
    // is unspecified and the caller should keep the whole buffer and
    // append more data before calling again. `out` accumulates state
    // across calls until Complete/Error -- callers must reuse the same
    // HttpRequest instance for the duration of one request's parse.
    ParseStatus parse(std::string_view buf, size_t& consumed, HttpRequest& out, std::string& error);

    // True while the request line and headers are still being read.
    bool inHead() const { return state_ == State::RequestLine || state_ == State::Headers; }

    // HTTP status the caller should answer with after parse() returned Error.
    int errorStatus() const { return errorStatus_; }

    void setMaxHeaderBytes(size_t n) { maxHeaderBytes_ = n; }
    void setMaxBodyBytes(size_t n) { maxBodyBytes_ = n; }
    // When enabled, discard exactly consumed bytes after every parse call,
    // including NeedMore. The default preserves the legacy whole-buffer API.
    void setIncrementalConsumption(bool enabled) { incrementalConsumption_ = enabled; }
    size_t writableBodyBytes(const HttpRequest& request) const
    {
        // Only as far as the body block reaches: a large body's block is
        // taken once its first bytes have arrived (see State::BodyLength).
        if (!incrementalConsumption_ || state_ != State::BodyLength)
            return 0;

        return std::min(contentLength_ - request.body.size(),
                        request.body.capacity() - request.body.size());
    }
    void commitBodyBytes(size_t bytes, HttpRequest& request)
    {
        if (bytes > writableBodyBytes(request))
            throw std::length_error("invalid direct body completion");

        request.body.setSize(request.body.size() + bytes);
    }
    // True once, when the headers carried `Expect: 100-continue` and the body
    // has not arrived yet: the client is holding the body back until it sees
    // an interim 100 response (libcurl waits a full second for it, then sends
    // anyway). The caller writes that response and reads on.
    bool takeExpectContinue()
    {
        const bool pending = expectContinue_ && state_ != State::Done;
        expectContinue_ = false;
        return pending;
    }

private:
    enum class State
    {
        RequestLine,
        Headers,
        BodyLength,
        ChunkSize,
        ChunkData,
        ChunkCRLF,
        ChunkTrailer,
        Done
    };

    // Where a piece of the head lies, as offsets from the start of the
    // request: lines are noted as they arrive and only turned into views
    // once the whole head is in hand and copied (see materializeHead).
    struct Span
    {
        uint32_t offset = 0;
        uint32_t length = 0;
    };
    struct HeaderSpan
    {
        Span name;
        Span value;
    };

    bool parseRequestLine(std::string_view line, HttpRequest& out, std::string& error);
    ParseStatus parseImpl(std::string_view buf, size_t& consumed, HttpRequest& out,
                          std::string& error);
    bool parseHeaderLine(std::string_view line, HttpRequest& out, std::string& error);
    void materializeHead(std::string_view buf, HttpRequest& out);
    bool finalizeHeaders(HttpRequest& out, std::string& error);
    ParseStatus needMore(size_t& consumed);
    size_t findLineEnd(std::string_view rest);
    Span spanOf(std::string_view piece) const
    {
        return {static_cast<uint32_t>(piece.data() - base_), static_cast<uint32_t>(piece.size())};
    }

    State state_ = State::RequestLine;
    int errorStatus_ = 400;
    size_t maxHeaderBytes_ = 64 * 1024;
    size_t maxBodyBytes_ = 256 * 1024 * 1024;  // generous default for image payloads
    size_t scanPos_ = 0;
    size_t lineScanned_ = 0;  // bytes of the current partial line already searched for CRLF
    size_t contentLength_ = 0;
    bool expectContinue_ = false;
    bool chunked_ = false;
    size_t chunkRemaining_ = 0;
    size_t headerBytes_ = 0;
    size_t headerCount_ = 0;
    size_t trailerBytes_ = 0;
    size_t trailerCount_ = 0;
    bool hasContentLength_ = false;
    bool hasTransferEncoding_ = false;
    bool hasHost_ = false;
    // Positions in HttpRequest::headers of the headers finalizeHeaders()
    // consults, noted as they are parsed so it need not scan for them.
    size_t connectionIndex_ = kNoHeader;
    size_t upgradeIndex_ = kNoHeader;
    size_t expectIndex_ = kNoHeader;
    bool incrementalConsumption_ = false;

    // The current parse() call's buffer start, which spans are relative to.
    const char* base_ = nullptr;
    Span targetSpan_;
    Span pathSpan_;
    Span querySpan_;
    bool hasQuery_ = false;
    bool pathEscaped_ = false;
    std::vector<HeaderSpan> headerSpans_;

    static constexpr size_t kNoHeader = static_cast<size_t>(-1);
};

std::string urlDecode(std::string_view in, bool plusAsSpace = false);

}  // namespace http
