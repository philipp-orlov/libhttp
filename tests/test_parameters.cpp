// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/http_context.hpp"
#include "http/query_binding.hpp"

#include <cstdio>
#include <stdexcept>

namespace {
struct BoundQuery
{
    http::FloatParameter confidence{"minConfidence", 0.9f, {0, 1}};
    http::SizeArrayParameter lengths;

    explicit BoundQuery(size_t bodySize) : lengths("lengths", {bodySize}, {1, 1024}, 3) {}
    auto parameters() { return std::tie(confidence, lengths); }
};

void check(bool condition)
{
    if (!condition)
        throw std::runtime_error("parameter assertion failed");
}

template <class Action>
void rejected(Action action)
{
    try {
        action();
    } catch (const http::ParameterError&) {
        return;
    }
    throw std::runtime_error("invalid parameter was accepted");
}

void parse(const std::string& query, std::initializer_list<http::Parameter*> parameters,
           http::QueryParseOptions options = {})
{
    http::HttpRequest request;
    request.queryString = query;
    http::HttpResponse response;
    http::HttpContext context(request, response);
    context.parseQuery(parameters, options);
}
}  // namespace

int main()
{
    try {
        size_t handled = 0;
        size_t completed = 0;
        float boundConfidence = 0;
        size_t boundLength = 0;
        std::function<void()> pending;
        auto route = http::bindQuery(
            [](const http::HttpContext& context) {
                return BoundQuery{context.request().body.size()};
            },
            [&](http::HttpContext&, const BoundQuery& query, std::function<void()> done) {
                ++handled;
                boundConfidence = query.confidence.value();
                boundLength = query.lengths.values().front();
                pending = std::move(done);
            },
            {http::UnknownParameters::Reject});
        auto invoke = [&](const std::string& query, size_t bodySize) {
            http::HttpRequest request;
            request.queryString = query;
            request.body.append(std::string(bodySize, 'x'));
            http::HttpResponse response;
            http::HttpContext context(request, response);
            route(context, [&] { ++completed; });
            if (response.status == 400)
                check(response.body.find("error") != std::string::npos);

            return response.status;
        };
        check(invoke("minConfidence=0.5&lengths=2", 2) == 200);
        check(handled == 1 && completed == 0 && boundConfidence == 0.5f && boundLength == 2);
        pending();
        check(completed == 1);
        for (const char* query : {"minConfidence=nan", "unknown=1",
                                  "minConfidence=0.2&minConfidence=0.8", "lengths=0"}) {
            check(invoke(query, 4) == 400);
            check(handled == 1);
        }
        check(completed == 5);
        check(invoke("", 7) == 200);
        check(handled == 2 && completed == 5 && boundConfidence == 0.9f && boundLength == 7);
        pending();
        check(completed == 6);
        auto throwingRoute =
            http::bindQuery([](const http::HttpContext&) { return BoundQuery{1}; },
                            [](http::HttpContext&, const BoundQuery&, std::function<void()>) {
                                throw http::ParameterError("handler", "not a binding failure");
                            });
        http::HttpRequest throwingRequest;
        http::HttpResponse throwingResponse;
        http::HttpContext throwingContext(throwingRequest, throwingResponse);
        rejected([&] { throwingRoute(throwingContext, [&] { ++completed; }); });
        check(completed == 6 && throwingResponse.status == 200);

        http::FloatParameter confidence("minConfidence", 0.9f, {0, 1});
        http::SizeArrayParameter lengths("lengths", {}, {1, 1024}, 3);
        http::StringParameter format("responseFormat", "json", {"json"});
        http::BoolParameter enabled("enabled");
        http::IntParameter count("count", 3, {-5, 5});
        parse("", {&confidence, &lengths, &format});
        check(confidence.value() == 0.9f && !confidence.parsed() && lengths.empty() &&
              format.value() == "json");
        parse("minConfidence=0.5&lengths=12%2C34&responseFormat=json&enabled=TrUe&count=-5",
              {&confidence, &lengths, &format, &enabled, &count});
        check(confidence.value() == 0.5f && confidence.parsed() && lengths.size() == 2 &&
              lengths.values()[1] == 34);
        check(enabled.value() && count.value() == -5);
        for (const char* value :
             {"", "NaN", "inf", "1e1000", "1.01", "-0.1", "0.5junk", " 0.5", "0.5 "})
            check(!confidence.tryParse(value) && confidence.value() == 0.5f);

        for (const char* value :
             {"", "0", "-1", "1,,2", "1,", ",1", "1,2,3,4", "1025", "18446744073709551616", "1x"})
            check(!lengths.tryParse(value) && lengths.size() == 2);

        for (const char* value : {"", "6", "1x", "9999999999999999999999", " 1"})
            check(!count.tryParse(value) && count.value() == -5);

        check(enabled.tryParse("0") && !enabled.value());
        check(enabled.tryParse("FALSE") && !enabled.value());
        check(!enabled.tryParse("") && !enabled.tryParse("yes"));
        check(!format.tryParse("xml") && !format.tryParse("JSON"));
        rejected([&] { parse("count=1&count=2", {&count}); });
        parse("count=1&count=invalid&ignored=x", {&count},
              {http::UnknownParameters::Ignore, http::DuplicateParameters::First});
        check(count.value() == 1);
        parse("count=1&count=2", {&count},
              {http::UnknownParameters::Ignore, http::DuplicateParameters::Last});
        check(count.value() == 2);
        rejected([&] { parse("unknown=1", {&count}, {http::UnknownParameters::Reject}); });
        parse("COUNT=4", {&count},
              {http::UnknownParameters::Reject, http::DuplicateParameters::Reject, true});
        check(count.value() == 4);
        rejected([&] {
            parse("count=1&COUNT=2", {&count},
                  {http::UnknownParameters::Reject, http::DuplicateParameters::Reject, true});
        });
        parse("COUNT=4", {&count});
        check(!count.parsed() && count.value() == 3);
        http::StringParameter required("name", "", {}, true);
        rejected([&] { parse("", {&required}); });
        parse("name=hello+world%21", {&required});
        check(required.value() == "hello world!");
        parse("name=", {&required});
        check(required.parsed() && required.value().empty());
        http::StringArrayParameter fields("fields", {}, {"x", "y"});
        check(fields.tryParse("x,y") && fields.size() == 2 && !fields.tryParse("x,z"));
        fields.reset();
        check(fields.empty() && !fields.parsed());
        http::DoubleParameter number("number");
        check(number.tryParse("-2.5e2") && number.value() == -250);
        count.reset();
        count.parseIf("other", "invalid");
        check(!count.parsed());
        count.parseIf("count", "2");
        check(count.parsed() && count.value() == 2);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts(
        "typed query parameters: defaults, ranges, arrays, policies and strict conversion passed");
}