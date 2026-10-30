// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <functional>
#include <tuple>
#include <utility>

#include "http/http_context.hpp"

namespace http {

template <class Factory, class Handler>
auto bindQuery(Factory factory, Handler handler, QueryParseOptions options = {})
{
    return [factory = std::move(factory), handler = std::move(handler), options](
               HttpContext& context, std::function<void()> done) {
        auto query = factory(context);
        try {
            std::apply([&](auto&... parameters) { context.parseQuery({&parameters...}, options); },
                       query.parameters());
        } catch (const ParameterError& error) {
            context.status(400).json(json::Json{{"error", error.what()}});
            done();
            return;
        }
        handler(context, std::as_const(query), std::move(done));
    };
}

}  // namespace http