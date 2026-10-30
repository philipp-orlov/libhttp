// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Doubles written into JSON and read from JSON and query parameters: exact
// round trips, the "C" locale's syntax whatever the process locale is, and no
// heap allocation per number. Built twice: once as the library is, and once
// with JSON_FP_CHARCONV=0 and HTTP_FP_CHARCONV=0, the snprintf/strtod path a
// standard library without floating-point <charconv> (GCC 9) takes.

#include "http/parameters.hpp"
#include "json/json.hpp"

#include <atomic>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>

namespace {
std::atomic<bool> g_counting{false};
std::atomic<size_t> g_allocations{0};
}  // namespace

void* operator new(size_t size)
{
    if (g_counting)
        ++g_allocations;

    if (void* pointer = std::malloc(size ? size : 1))
        return pointer;

    throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept
{
    std::free(pointer);
}
void operator delete(void* pointer, size_t) noexcept
{
    std::free(pointer);
}

namespace {
#ifndef FLOAT_FORMATS_LABEL
#define FLOAT_FORMATS_LABEL "<charconv>"
#endif

void check(bool condition, int code, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "FAILED (%s): %s\n", FLOAT_FORMATS_LABEL, what);
        std::_Exit(code);
    }
}

bool sameDouble(double left, double right)
{
    return std::memcmp(&left, &right, sizeof left) == 0;
}

void roundTrips()
{
    const double values[] = {0.1,
                             0.2,
                             1.0 / 3,
                             2.5e-8,
                             123456.789,
                             -0.0,
                             1e300,
                             -1e-300,
                             4.9e-324,
                             2.2250738585072014e-308,
                             1.7976931348623157e308,
                             0.93,
                             12.5,
                             1e21,
                             9007199254740993.0,
                             -273.15};
    for (const double value : values) {
        const std::string text = json::Json(value).dump();
        json::Json parsed;
        std::string error;
        check(json::Json::parse(text, parsed, error), 1, "parse what dump wrote");
        check(sameDouble(parsed.asDouble(), value), 2, "double round trip");
        double parameter = 0;
        check(http::internal::parseFloatingText(text, parameter) && sameDouble(parameter, value), 3,
              "parameter round trip");
    }
    // Shortest forms, the same on both paths.
    const std::pair<double, const char*> shortest[] = {{0.1, "0.1"},    {12.5, "12.5"},
                                                       {0.93, "0.93"},  {2.5e-8, "2.5e-08"},
                                                       {1e21, "1e+21"}, {123456.789, "123456.789"},
                                                       {-0.0, "-0"}};
    for (const auto& entry : shortest)
        check(json::Json(entry.first).dump() == entry.second, 4, entry.second);

    json::Json parsed;
    std::string error;
    check(!json::Json::parse("1e999", parsed, error), 5, "overflow refused");
    check(!json::Json::parse("1e-400", parsed, error), 6, "underflow to zero refused");
}

void parameters()
{
    double value = 0;
    const std::pair<const char*, double> accepted[] = {
        {"0.5", 0.5}, {"+0.5", 0.5}, {"-2.5e2", -250}, {".5", 0.5}, {"5.", 5}, {"1E3", 1000}};
    for (const auto& entry : accepted)
        check(http::internal::parseFloatingText(entry.first, value) && value == entry.second, 7,
              entry.first);

    for (const char* text :
         {"", " 0.5", "0.5 ", "0x1p3", "inf", "nan", "1e1000", "0.5junk", "1e", "+", "+-1", "--1"})
        check(!http::internal::parseFloatingText(text, value), 8, text);

    float single = 0;
    check(http::internal::parseFloatingText("0.9", single) && single == 0.9f, 9, "float");
    check(!http::internal::parseFloatingText("1e39", single), 10, "float overflow");
}

void locale()
{
    // A process that switched to a locale with a decimal comma.
    for (const char* name : {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "ru_RU.UTF-8"}) {
        if (!std::setlocale(LC_NUMERIC, name))
            continue;

        check(json::Json(0.5).dump() == "0.5", 11, "decimal point under a comma locale");
        json::Json parsed;
        std::string error;
        check(json::Json::parse("[0.25]", parsed, error) && parsed[0].asDouble() == 0.25, 12,
              "parse under a comma locale");
        double value = 0;
        check(http::internal::parseFloatingText("0.75", value) && value == 0.75, 13,
              "parameter under a comma locale");
        std::setlocale(LC_NUMERIC, "C");
        std::printf("float formats (%s): checked under %s\n", FLOAT_FORMATS_LABEL, name);
        return;
    }
}

void allocations()
{
    json::Json document = json::Json::array();
    for (int index = 0; index < 1000; ++index)
        document.push_back(index * 0.001 + 0.0625 / (index + 1));
    // Parsed arrays come from the JSON buffer pools, which keep arrays of up
    // to 256 elements.
    json::Json small = json::Json::array();
    for (int index = 0; index < 200; ++index)
        small.push_back(index * 0.37 + 0.001);
    const std::string smallText = small.dump();
    std::string text;
    json::Json parsed;
    std::string error;
    double value = 0;
    for (int round = 0; round < 3; ++round) {
        text.clear();
        document.dump(text);
        check(json::Json::parse(smallText, parsed, error), 14, "parse the array");
    }
    g_counting = true;
    for (int round = 0; round < 100; ++round) {
        text.clear();
        document.dump(text);
        json::Json::parse(smallText, parsed, error);
        http::internal::parseFloatingText("0.123456789", value);
    }
    g_counting = false;
    std::printf(
        "float formats (%s): %zu allocations over 100 x (dump of 1000 doubles, parse of "
        "200)\n",
        FLOAT_FORMATS_LABEL, g_allocations.load());
    check(g_allocations == 0, 15, "no allocation per number");
}
}  // namespace

int main()
{
    roundTrips();
    parameters();
    locale();
    allocations();
    std::printf("float formats (%s): round trips, syntax, locale and zero allocations passed\n",
                FLOAT_FORMATS_LABEL);
}
