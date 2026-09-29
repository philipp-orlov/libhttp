// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/parameters.hpp"

namespace http {

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