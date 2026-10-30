// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// JSON: parsing arbitrary text must be safe, and what parses must serialize to
// something that parses back to the same document.

#include "json/json.hpp"

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    json::Json value;
    std::string error;
    if (!json::Json::parse(std::string_view(reinterpret_cast<const char*>(data), size), value,
                           error))
        return 0;

    const std::string text = value.dump();
    if (value.measure() != text.size())
        std::abort();

    json::Json again;
    if (!json::Json::parse(text, again, error) || again.dump() != text)
        std::abort();

    // Integer accessors never invoke undefined behaviour, whatever the number is.
    if (value.isNumber()) {
        long long integer = 0;
        (void)value.tryInt64(integer);
        try {
            (void)value.asInt();
        } catch (const std::range_error&) {
        }
    }
    json::Json::trimCache();
    return 0;
}

std::vector<std::string> fuzzSeeds()
{
    return {
        R"({"a":1,"b":[true,false,null,"x\u00e9\ud83d\ude00"],"c":{"d":-0.5e3}})",
        "[1,2,3,9007199254740993,-9223372036854775808,1e999]",
        "\"unterminated",
        R"({"k0":0,"k1":1,"k2":2,"k3":3,"k4":4,"k5":5,"k6":6,"k7":7,"k8":8,"k9":9,"k10":10,"k11":11,"k12":12,"k13":13,"k14":14,"k15":15,"k16":16,"k17":17,"k18":18,"k19":19,"k20":20,"k21":21,"k22":22,"k23":23,"k24":24,"k25":25,"k26":26,"k27":27,"k28":28,"k29":29,"k30":30,"k31":31,"k32":32,"k33":33,"k31":0})",
        std::string(70, '[') + std::string(70, ']'),
        "-0",
        "  \t\n{ }  ",
    };
}
