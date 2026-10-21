// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <functional>
#include <string>
#include <type_traits>

#include "http/http_context.hpp"
#include "http/http_types.hpp"
#include "http/route_pattern.hpp"

namespace http {

// A handler receives the request/response context and a `done` callback
// it must invoke exactly once when the response is ready. Synchronous
// handlers (the common case: read the request, fill the response, return)
// call `done()` at the end; a handler that offloads CPU-bound work to a
// thread pool captures `done` and calls it later, from
// wherever the work finishes, once it has hopped back onto the
// connection's own IoLoop via IoLoop::post (see Server::pool()).
using Handler = std::function<void(HttpContext&, std::function<void()> done)>;

// Default values for trailing optional route parameters, keyed by
// parameter name -- see RoutePattern's documentation for the
// "/api/{controller}/{action}" example.
using RouteDefaults = std::vector<std::pair<std::string, std::string>>;

// Fluent route table:
//   router.onGet("/health", [](HttpContext& context){ context.json(...); })
//         .onPost("/upload/{name}", [](HttpContext& context, std::function<void()> done){ ... });
//         .onGet("/api/{controller}/{action}", handler, {{"action", "index"}});
// Both plain `(HttpContext&)` handlers and full `(HttpContext&, done)`
// handlers are accepted -- the former is wrapped to call `done()`
// immediately after returning. The trailing `defaultValues` parameter is
// optional; when given, it makes the corresponding trailing route
// parameters optional in the URL.
class Router
{
public:
    template <class F>
    Router& map(HttpMethod method, std::string pattern, F&& handlerFunction,
                RouteDefaults defaultValues = {})
    {
        table_.add(method, std::move(pattern), wrap(std::forward<F>(handlerFunction)),
                   std::move(defaultValues));
        return *this;
    }

    template <class F>
    Router& onGet(std::string pattern, F&& handlerFunction, RouteDefaults defaultValues = {})
    {
        return map(HttpMethod::Get, std::move(pattern), std::forward<F>(handlerFunction),
                   std::move(defaultValues));
    }
    template <class F>
    Router& onHead(std::string pattern, F&& handlerFunction, RouteDefaults defaultValues = {})
    {
        return map(HttpMethod::Head, std::move(pattern), std::forward<F>(handlerFunction),
                   std::move(defaultValues));
    }
    template <class F>
    Router& onPost(std::string pattern, F&& handlerFunction, RouteDefaults defaultValues = {})
    {
        return map(HttpMethod::Post, std::move(pattern), std::forward<F>(handlerFunction),
                   std::move(defaultValues));
    }
    template <class F>
    Router& onPut(std::string pattern, F&& handlerFunction, RouteDefaults defaultValues = {})
    {
        return map(HttpMethod::Put, std::move(pattern), std::forward<F>(handlerFunction),
                   std::move(defaultValues));
    }
    template <class F>
    Router& onPatch(std::string pattern, F&& handlerFunction, RouteDefaults defaultValues = {})
    {
        return map(HttpMethod::Patch, std::move(pattern), std::forward<F>(handlerFunction),
                   std::move(defaultValues));
    }
    template <class F>
    Router& onDelete(std::string pattern, F&& handlerFunction, RouteDefaults defaultValues = {})
    {
        return map(HttpMethod::Delete, std::move(pattern), std::forward<F>(handlerFunction),
                   std::move(defaultValues));
    }

    const Handler* match(HttpMethod method, std::string_view path,
                         std::vector<std::pair<std::string, std::string>>& outParams) const
    {
        return table_.match(method, path, outParams);
    }

    std::vector<HttpMethod> allowedMethods(std::string_view path) const
    {
        return table_.allowedMethods(path);
    }

private:
    // Checked in this order deliberately: a std::bind result has a
    // variadic call operator that silently accepts (and ignores) more
    // arguments than it has placeholders for, so a member function bound
    // with one placeholder (the common sync-handler case, e.g.
    // std::bind(&Controller::handle, &controller, std::placeholders::_1))
    // would be misdetected as also invocable with (HttpContext&, done) --
    // and calling it that way would silently drop `done`, so the response
    // would never be sent. Checking the one-argument form first, and only
    // falling back to the two-argument form when that fails, resolves
    // correctly for both plain lambdas and std::bind-wrapped member
    // functions of either arity.
    template <class F>
    static Handler wrap(F&& handlerFunction)
    {
        if constexpr (std::is_invocable_v<F, HttpContext&>) {
            return [handlerFunction = std::forward<F>(handlerFunction)](
                       HttpContext& context, std::function<void()> done) {
                handlerFunction(context);
                done();
            };
        } else {
            static_assert(std::is_invocable_v<F, HttpContext&, std::function<void()>>,
                          "handler must be callable as (HttpContext&) or (HttpContext&, "
                          "std::function<void()>)");
            return Handler(std::forward<F>(handlerFunction));
        }
    }

    RouteTable<Handler> table_;
};

}  // namespace http
