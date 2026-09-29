// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "http/http_types.hpp"

namespace http {

// A compiled route pattern like "/api/{controller}/{action}". Segments
// written as "{name}" capture that path segment; everything else must
// match literally. Shared by the HTTP route table and the WebSocket route
// table so both get the same {param} syntax for free.
//
// A trailing parameter segment can be made optional by supplying a
// default value for it (see `defaultValues`, keyed by parameter name):
// "/api/{controller}/{action}" registered with defaultValues
// {{"action", "index"}} matches both "/api/products/list" (action="list")
// and "/api/products" (action defaults to "index"). Only trailing
// segments can be optional this way -- a request path can omit them from
// the end, never from the middle.
class RoutePattern
{
public:
    explicit RoutePattern(std::string_view pattern,
                          std::vector<std::pair<std::string, std::string>> defaultValues = {});

    bool match(std::string_view path,
               std::vector<std::pair<std::string, std::string>>& outParams) const;

private:
    struct Segment
    {
        std::string literal;
        bool isParam = false;
        std::string paramName;
    };

    const std::string* defaultFor(const std::string& paramName) const;

    std::vector<Segment> segments_;
    std::vector<std::pair<std::string, std::string>> defaultValues_;
};

// Generic method+pattern -> handler table, templated on the handler type
// so it serves both http::Handler (HTTP) and WebSocketHandler.
template <class HandlerT>
class RouteTable
{
public:
    void add(HttpMethod method, std::string pattern, HandlerT handler,
             std::vector<std::pair<std::string, std::string>> defaultValues = {})
    {
        entries_.push_back(
            Entry{method, RoutePattern(pattern, std::move(defaultValues)), std::move(handler)});
    }

    const HandlerT* match(HttpMethod method, std::string_view path,
                          std::vector<std::pair<std::string, std::string>>& outParams) const
    {
        for (auto& e : entries_) {
            if (e.method != method)
                continue;

            outParams.clear();
            if (e.pattern.match(path, outParams))
                return &e.handler;
        }
        return nullptr;
    }

    // Every method registered for a path, in registration order and
    // deduplicated. Empty means the path itself is unknown; non-empty for a
    // method not in it is a 405, and it is also the `Allow` header's value.
    std::vector<HttpMethod> allowedMethods(std::string_view path) const
    {
        std::vector<HttpMethod> methods;
        std::vector<std::pair<std::string, std::string>> params;
        for (auto& e : entries_) {
            params.clear();
            if (!e.pattern.match(path, params))
                continue;

            if (std::find(methods.begin(), methods.end(), e.method) == methods.end()) {
                methods.push_back(e.method);
            }
        }
        return methods;
    }

private:
    struct Entry
    {
        HttpMethod method;
        RoutePattern pattern;
        HandlerT handler;
    };
    std::vector<Entry> entries_;
};

}  // namespace http
