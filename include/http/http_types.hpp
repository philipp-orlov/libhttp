// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "http/buffer.hpp"

namespace http {

enum class HttpMethod
{
    Get,
    Head,
    Post,
    Put,
    Patch,
    Delete,
    Options,
    Unknown
};

inline HttpMethod parseMethod(std::string_view m)
{
    if (m == "GET")
        return HttpMethod::Get;

    if (m == "HEAD")
        return HttpMethod::Head;

    if (m == "POST")
        return HttpMethod::Post;

    if (m == "PUT")
        return HttpMethod::Put;

    if (m == "PATCH")
        return HttpMethod::Patch;

    if (m == "DELETE")
        return HttpMethod::Delete;

    if (m == "OPTIONS")
        return HttpMethod::Options;

    return HttpMethod::Unknown;
}

inline const char* methodName(HttpMethod m)
{
    switch (m) {
        case HttpMethod::Get: return "GET";
        case HttpMethod::Head: return "HEAD";
        case HttpMethod::Post: return "POST";
        case HttpMethod::Put: return "PUT";
        case HttpMethod::Patch: return "PATCH";
        case HttpMethod::Delete: return "DELETE";
        case HttpMethod::Options: return "OPTIONS";
        default: return "UNKNOWN";
    }
}

inline const char* reasonPhrase(int status)
{
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 421: return "Misdirected Request";
        case 426: return "Upgrade Required";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 503: return "Service Unavailable";
        case 505: return "HTTP Version Not Supported";
        default: return "Unknown";
    }
}

// ASCII only, which is all a header name or token can be; avoids the
// locale lookup behind std::tolower on every character.
inline unsigned char asciiLower(unsigned char value)
{
    return (value >= 'A' && value <= 'Z') ? static_cast<unsigned char>(value | 0x20) : value;
}

inline bool equalsIgnoreCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i) {
        if (asciiLower(static_cast<unsigned char>(a[i])) !=
            asciiLower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Small header container. Requests/responses carry a handful of headers,
// so a flat vector with linear scan beats a hash map's allocation and
// hashing overhead here. Entries cleared by clear() keep their strings'
// storage and are reused by the next add(), so a connection parsing and
// answering requests for hours does so without a header allocation.
//
// This is the response's header list, and what it holds is written to the
// wire verbatim, so set()/add() refuse (std::invalid_argument) a name that
// is not an RFC 9110 token and a value with a control character other than
// HTAB -- a header built from client-supplied data cannot split the response.
inline bool isHeaderName(std::string_view name)
{
    if (name.empty())
        return false;

    for (const char c : name) {
        const unsigned char v = static_cast<unsigned char>(c);
        const bool token = (v >= '0' && v <= '9') || (v >= 'A' && v <= 'Z') ||
                           (v >= 'a' && v <= 'z') ||
                           (v < 128 && std::string_view("!#$%&'*+-.^_`|~").find(c) !=
                                           std::string_view::npos);
        if (!token)
            return false;
    }
    return true;
}

inline bool isHeaderValue(std::string_view value)
{
    for (const char c : value) {
        const unsigned char v = static_cast<unsigned char>(c);
        if ((v < 32 && v != '\t') || v == 127)
            return false;
    }
    return true;
}

class HeaderList
{
public:
    void set(std::string_view name, std::string_view value)
    {
        check(name, value);
        for (auto& h : items_) {
            if (equalsIgnoreCase(h.first, name)) {
                h.second.assign(value.data(), value.size());
                return;
            }
        }
        addChecked(name, value);
    }

    void add(std::string_view name, std::string_view value)
    {
        check(name, value);
        addChecked(name, value);
    }

    std::string_view get(std::string_view name) const
    {
        for (auto& h : items_) {
            if (equalsIgnoreCase(h.first, name))
                return h.second;
        }
        return {};
    }

    bool has(std::string_view name) const
    {
        for (auto& h : items_) {
            if (equalsIgnoreCase(h.first, name))
                return true;
        }
        return false;
    }

    const std::vector<std::pair<std::string, std::string>>& items() const { return items_; }
    size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }

    void clear()
    {
        for (auto& h : items_) {
            if (spare_.size() < kMaxSpare)
                spare_.push_back(std::move(h));
        }
        items_.clear();
    }

private:
    static void check(std::string_view name, std::string_view value)
    {
        if (!isHeaderName(name))
            throw std::invalid_argument("invalid response header name");

        if (!isHeaderValue(value))
            throw std::invalid_argument("invalid response header value");
    }

    void addChecked(std::string_view name, std::string_view value)
    {
        if (!spare_.empty()) {
            items_.push_back(std::move(spare_.back()));
            spare_.pop_back();
        } else {
            items_.emplace_back();
        }
        auto& h = items_.back();
        h.first.assign(name.data(), name.size());
        h.second.assign(value.data(), value.size());
    }

    // Enough for any ordinary request or response; a one-off flood of
    // headers is not worth keeping storage for.
    static constexpr size_t kMaxSpare = 32;

    std::vector<std::pair<std::string, std::string>> items_;
    std::vector<std::pair<std::string, std::string>> spare_;
};

// Header names and values of a request, as views into HttpRequest::head.
class HeaderViewList
{
public:
    using Item = std::pair<std::string_view, std::string_view>;

    void add(std::string_view name, std::string_view value) { items_.emplace_back(name, value); }

    std::string_view get(std::string_view name) const
    {
        for (auto& h : items_) {
            if (equalsIgnoreCase(h.first, name))
                return h.second;
        }
        return {};
    }

    bool has(std::string_view name) const
    {
        for (auto& h : items_) {
            if (equalsIgnoreCase(h.first, name))
                return true;
        }
        return false;
    }

    const std::vector<Item>& items() const { return items_; }
    size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }
    void clear() { items_.clear(); }

private:
    std::vector<Item> items_;
};

// A parsed request. The request line and headers are views into `head`,
// the request's own copy of its head bytes (made once, when the head is
// complete), so they stay valid for as long as the request does -- until
// the handler's `done` -- without a string per field or per header.
struct HttpRequest
{
    HttpMethod method = HttpMethod::Unknown;
    std::string_view path;         // decoded path, no query string
    std::string_view rawTarget;    // full request-target as sent
    std::string_view queryString;  // raw query string (without '?')
    int versionMajor = 1;
    int versionMinor = 1;
    HeaderViewList headers;
    Buffer body;  // request body; pool-backed, grown in place as it's parsed
    bool keepAlive = true;
    bool isUpgrade = false;
    std::string_view upgradeTo;  // e.g. "websocket"

    // Storage behind the views above: the head as received, and the path
    // once decoded when the target carried percent-escapes (otherwise
    // `path` points into `head` too). Reused across requests.
    std::string head;
    std::string decodedPath;
};

struct HttpResponse
{
    int status = 200;
    HeaderList headers;
    std::string body;

    void setHeader(std::string_view name, std::string_view value) { headers.set(name, value); }
};

}  // namespace http
