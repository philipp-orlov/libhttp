// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/parameters.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>

// Floating-point std::from_chars arrived in libstdc++ 11 (JetPack 5 ships
// GCC 9). Without it, strtof/strtod on a stack copy in the "C" locale.
// Defining HTTP_FP_CHARCONV=0 forces that fallback (its tests do).
#ifndef HTTP_FP_CHARCONV
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
#define HTTP_FP_CHARCONV 1
#else
#define HTTP_FP_CHARCONV 0
#endif
#endif

#if !HTTP_FP_CHARCONV && !defined(_WIN32)
#include <locale.h>
#endif

namespace http {

namespace {
template <class Value>
bool parseFloating(std::string_view text, Value& value)
{
    // A leading '+' was accepted by the iostreams parse this replaces.
    if (text.size() > 1 && text.front() == '+' && text[1] != '-' && text[1] != '+')
        text.remove_prefix(1);

    if (text.empty())
        return false;

#if HTTP_FP_CHARCONV
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        return false;
#else
    // strtod also takes leading blanks, hex floats, "inf" and "nan", which
    // the other path refuses; only the plain decimal characters pass here.
    char token[128];
    if (text.size() >= sizeof token)
        return false;

    for (const char c : text) {
        if (!((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-'))
            return false;
    }
    std::memcpy(token, text.data(), text.size());
    token[text.size()] = '\0';
#ifndef _WIN32
    static const locale_t classic = newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(0));
    const locale_t previous = classic ? uselocale(classic) : static_cast<locale_t>(0);
#endif
    char* end = nullptr;
    errno = 0;
    if constexpr (std::is_same_v<Value, float>)
        value = std::strtof(token, &end);
    else if constexpr (std::is_same_v<Value, double>)
        value = std::strtod(token, &end);
    else
        value = std::strtold(token, &end);
    // ERANGE also flags a subnormal result, which from_chars accepts; only an
    // underflow to zero (and an overflow, which is not finite) is refused.
    const bool whole = end == token + text.size() && !(errno == ERANGE && value == 0);
#ifndef _WIN32
    if (previous)
        uselocale(previous);
#endif
    if (!whole)
        return false;
#endif
    return std::isfinite(value);
}
}  // namespace

bool internal::parseFloatingText(std::string_view text, float& value)
{
    return parseFloating(text, value);
}

bool internal::parseFloatingText(std::string_view text, double& value)
{
    return parseFloating(text, value);
}

bool internal::parseFloatingText(std::string_view text, long double& value)
{
    return parseFloating(text, value);
}

bool internal::parameterTextEquals(std::string_view left, std::string_view right,
                                   bool caseInsensitive)
{
    if (left.size() != right.size())
        return false;

    auto lower = [](unsigned char character) {
        return character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character;
    };
    for (size_t index = 0; index < left.size(); ++index) {
        if (caseInsensitive ? lower(left[index]) != lower(right[index])
                            : left[index] != right[index])
            return false;
    }
    return true;
}

bool Parameter::is(std::string_view name, bool caseInsensitive) const
{
    return internal::parameterTextEquals(name_, name, caseInsensitive);
}

void Parameter::parse(std::string_view text)
{
    if (!parseValue(text))
        throw ParameterError(name_, "invalid value or outside the allowed range");

    parsed_ = true;
}

bool Parameter::tryParse(std::string_view text)
{
    try {
        parse(text);
        return true;
    } catch (const ParameterError&) {
        return false;
    }
}

void Parameter::parseIf(std::string_view name, std::string_view text)
{
    if (is(name))
        parse(text);
}

}  // namespace http