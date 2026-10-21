// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "http/http_types.hpp"
#include "http/io_loop.hpp"
#include "http/parameters.hpp"
#include "json/json.hpp"

namespace http {

struct CookieOptions
{
    std::string name;
    std::string value;
    std::string path = "/";
    std::string domain;
    int maxAgeSeconds = -1;  // -1 = session cookie (no Max-Age)
    bool secure = false;
    bool httpOnly = true;
    std::string sameSite = "Lax";  // "Strict" | "Lax" | "None"
};

// Per-request context, in the spirit of ASP.NET's HttpContext: one object
// exposing the parsed request, mutable response, and convenience accessors
// for query parameters, cookies, and route parameters. Query/cookie
// parsing is lazy -- most handlers only touch one or two, so we avoid
// paying to parse the rest.
class HttpContext
{
public:
    HttpContext(HttpRequest& request, HttpResponse& response)
        : request_(request), response_(response)
    {}

    const HttpRequest& request() const { return request_; }
    HttpRequest& request() { return request_; }
    HttpResponse& response() { return response_; }

    std::string_view query(std::string_view name) const;
    bool hasQuery(std::string_view name) const;
    const std::vector<std::pair<std::string, std::string>>& queryParams() const;
    void parseQuery(std::initializer_list<Parameter*> parameters,
                    QueryParseOptions options = {}) const;

    std::string_view cookie(std::string_view name) const;
    bool hasCookie(std::string_view name) const;

    // Route parameters captured from a pattern like "/upload/{name}".
    std::string_view routeParam(std::string_view name) const;
    void setRouteParams(std::vector<std::pair<std::string, std::string>> params)
    {
        // Swapped rather than assigned so the caller gets this context's old
        // storage back to reuse for the next request.
        routeParams_.swap(params);
    }

    const std::string& remoteAddress() const { return remote_ ? *remote_ : remoteAddr_; }
    void setRemoteAddress(std::string addr)
    {
        remoteAddr_ = std::move(addr);
        remote_ = nullptr;
    }

    // The IoLoop servicing this connection. A handler that offloads work
    // to a thread pool must post() back onto this loop before touching
    // the context or response again -- HttpContext is not thread-safe.
    IoLoop& loop() const { return *loop_; }
    void setLoop(IoLoop* loop) { loop_ = loop; }

    // --- Fluent response helpers ---
    // Throws std::invalid_argument for a code outside 100..999, which no
    // status line can carry.
    HttpContext& status(int code)
    {
        if (code < 100 || code > 999)
            throw std::invalid_argument("invalid HTTP status code");

        response_.status = code;
        return *this;
    }
    HttpContext& header(std::string_view name, std::string_view value)
    {
        response_.setHeader(name, value);
        return *this;
    }
    HttpContext& setCookie(const CookieOptions& options);

    HttpContext& text(std::string_view body, std::string_view contentType = "text/plain; charset=utf-8");
    HttpContext& json(const json::Json& body);
    HttpContext& bytes(const void* data, size_t len,
                       std::string_view contentType = "application/octet-stream");
    // A body already in a pooled block, for example one leased from
    // Server::bufferPool() on a worker and filled there: it is sent from that
    // block without a copy and the block goes back to the pool once written.
    HttpContext& bytes(Buffer&& body, std::string_view contentType = "application/octet-stream");
    HttpContext& noContent()
    {
        response_.status = 204;
        return *this;
    }

private:
    friend class Session;
    using Pairs = std::vector<std::pair<std::string, std::string>>;

    void ensureQueryParsed() const;
    void ensureCookiesParsed() const;

    // --- used by the Session that owns this context for all its requests ---
    // Back to the state of a fresh context for the next request; parsed
    // query parameters and cookies keep their strings for reuse.
    void reset();
    // Frees reused strings beyond maxBytes of capacity; returns the capacity kept.
    size_t trimSpare(size_t maxBytes);
    void swapRouteParams(Pairs& params) { routeParams_.swap(params); }
    // remoteAddress() reads `address`, which outlives the context's use.
    void referRemoteAddress(const std::string& address) { remote_ = &address; }

    HttpRequest& request_;
    HttpResponse& response_;

    mutable bool queryParsed_ = false;
    mutable Pairs queryParams_;
    mutable bool cookiesParsed_ = false;
    mutable Pairs cookies_;
    // Entries of earlier requests, reused by the next parse.
    mutable Pairs spare_;

    Pairs routeParams_;
    std::string remoteAddr_;
    const std::string* remote_ = nullptr;
    IoLoop* loop_ = nullptr;
};

}  // namespace http
