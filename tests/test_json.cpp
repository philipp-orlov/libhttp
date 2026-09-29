// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "json/json.hpp"
#include "json/utf8.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

int main()
{
    const std::vector<std::string> invalid = {"{\"x\":1,\"x\":2}",
                                              "1e999",
                                              "\"\\udc00\"",
                                              "\"\\ud800\"",
                                              std::string(65, '[') + "0" + std::string(65, ']'),
                                              std::string("\"\xc0\x80\"", 4)};
    json::Json output;
    std::string error;
    for (const auto& input : invalid)
        if (json::Json::parse(input, output, error))
            return 1;

    if (!json::Json::parse("{\"value\":1.25,\"name\":\"\\ud83d\\ude00\"}", output, error))
        return 2;

    json::Json again;
    if (!json::Json::parse(output.dump(), again, error))
        return 3;

    if (json::isValidUtf8(std::string_view("\xed\xa0\x80", 3)) ||
        json::isValidUtf8(std::string_view("\xf4\x90\x80\x80", 4)))
        return 4;

    // Integers keep every digit; a fraction or exponent makes a double.
    {
        json::Json number;
        long long integer = 0;
        if (!json::Json::parse("9007199254740993", number, error) || !number.isInteger() ||
            number.asInt() != 9007199254740993LL || number.dump() != "9007199254740993")
            return 5;

        if (!json::Json::parse("-9223372036854775808", number, error) ||
            number.asInt() != std::numeric_limits<long long>::min())
            return 6;

        // Beyond int64 the token is a double; asInt refuses it.
        if (!json::Json::parse("9223372036854775808", number, error) || number.isInteger())
            return 7;

        bool refused = false;
        try {
            number.asInt();
        } catch (const std::range_error&) {
            refused = true;
        }
        if (!refused || number.tryInt64(integer))
            return 8;

        if (!json::Json::parse("1e3", number, error) || number.isInteger() ||
            !number.tryInt64(integer) || integer != 1000)
            return 9;

        if (!json::Json::parse("2.5", number, error) || number.tryInt64(integer) ||
            number.asInt() != 2)
            return 10;

        bool tooBig = false;
        try {
            json::Json::parse("1e30", number, error);
            number.asInt();
        } catch (const std::range_error&) {
            tooBig = true;
        }
        if (!tooBig)
            return 11;

        // Checked access for client-supplied values.
        json::Json five(5);
        bool bounded = false;
        try {
            five.asIntChecked(6, 9);
        } catch (const std::range_error&) {
            bounded = true;
        }
        if (!bounded || five.asIntChecked(0, 9) != 5)
            return 12;

        bool notIntegral = false;
        try {
            json::Json(2.5).asIntChecked(0, 9);
        } catch (const std::range_error&) {
            notIntegral = true;
        }
        if (!notIntegral)
            return 13;

        // Built-in integer types are exact too, and doubles still print as before.
        if (json::Json(9007199254740993LL).dump() != "9007199254740993" ||
            json::Json(0.5).dump() != "0.5" || json::Json(3).asDouble() != 3.0)
            return 14;

        // Above int64 an unsigned value falls back to a double (approximate, still finite).
        if (json::Json(18446744073709551615ULL).isInteger() ||
            json::Json(18446744073709551615ULL).asDouble() != 18446744073709551616.0)
            return 18;

        // Negative zero keeps its sign, as a double.
        if (!json::Json::parse("-0", number, error) || number.isInteger() ||
            number.dump() != json::Json(-0.0).dump() || !std::signbit(number.asDouble()))
            return 19;
    }
    // Objects with many keys: duplicates are still found, quickly.
    {
        std::string large = "{";
        for (int index = 0; index < 4000; ++index)
            large += (index ? ",\"k" : "\"k") + std::to_string(index) + "\":" + std::to_string(index);

        json::Json object;
        if (!json::Json::parse(large + "}", object, error) || object.asObject().size() != 4000 ||
            object.find("k3999")->asInt() != 3999)
            return 15;

        for (const char* duplicate : {",\"k0\":1}", ",\"k31\":1}", ",\"k32\":1}", ",\"k3999\":1}"})
            if (json::Json::parse(large + duplicate, object, error) ||
                error.find("duplicate") == std::string::npos)
                return 16;

        if (json::Json::parse(large + ",\"k4000\":1}", object, error) == false)
            return 17;
    }

    std::puts("JSON: finite numbers, duplicate keys, nesting and UTF-8 limits passed");
}
