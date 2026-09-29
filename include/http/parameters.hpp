// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace http {

class ParameterError : public std::invalid_argument
{
public:
    ParameterError(std::string name, const std::string& reason)
        : std::invalid_argument("query parameter '" + name + "': " + reason), name_(std::move(name))
    {}
    const std::string& name() const { return name_; }

private:
    std::string name_;
};

enum class UnknownParameters
{
    Ignore,
    Reject
};
enum class DuplicateParameters
{
    Reject,
    First,
    Last
};

struct QueryParseOptions
{
    UnknownParameters unknown = UnknownParameters::Ignore;
    DuplicateParameters duplicates = DuplicateParameters::Reject;
    bool caseInsensitiveNames = false;
};

class Parameter
{
public:
    explicit Parameter(std::string name, bool required = false)
        : name_(std::move(name)), required_(required)
    {}
    virtual ~Parameter() = default;
    const std::string& name() const { return name_; }
    bool required() const { return required_; }
    bool parsed() const { return parsed_; }
    bool is(std::string_view name, bool caseInsensitive = false) const;
    void parse(std::string_view text);
    bool tryParse(std::string_view text);
    void parseIf(std::string_view name, std::string_view text);
    void reset()
    {
        resetValue();
        parsed_ = false;
    }

private:
    virtual bool parseValue(std::string_view text) = 0;
    virtual void resetValue() = 0;
    std::string name_;
    bool required_;
    bool parsed_ = false;
};

namespace internal {
bool parameterTextEquals(std::string_view left, std::string_view right, bool caseInsensitive);

template <class Value>
bool parseParameterValue(std::string_view text, Value& value)
{
    if constexpr (std::is_same_v<Value, std::string>) {
        value.assign(text);
        return true;
    } else if constexpr (std::is_same_v<Value, bool>) {
        if (text == "1" || parameterTextEquals(text, "true", true)) {
            value = true;
            return true;
        }
        if (text == "0" || parameterTextEquals(text, "false", true)) {
            value = false;
            return true;
        }
        return false;
    } else if constexpr (std::is_integral_v<Value>) {
        if (text.empty())
            return false;

        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size();
    } else {
        static_assert(std::is_floating_point_v<Value>, "unsupported parameter value type");
        std::istringstream input{std::string(text)};
        input.imbue(std::locale::classic());
        input >> std::noskipws >> value;
        return input && input.peek() == std::char_traits<char>::eof() && std::isfinite(value);
    }
}
}  // namespace internal

template <class Value>
struct ParameterRange
{
    Value minimum = std::numeric_limits<Value>::lowest();
    Value maximum = std::numeric_limits<Value>::max();
    bool contains(Value value) const { return value >= minimum && value <= maximum; }
};

template <>
struct ParameterRange<std::string>
{
    ParameterRange(std::initializer_list<std::string> allowed = {}) : choices(allowed) {}
    // GCC 9 cannot convert a braced list of string literals to the
    // std::string overload above when it arrives as a default-carrying
    // constructor argument (`StringParameter f{"n", "", {"a", "b"}}`); newer
    // compilers can. Spelling the literal form out keeps both happy.
    ParameterRange(std::initializer_list<const char*> allowed) : choices(allowed.begin(), allowed.end()) {}
    std::vector<std::string> choices;
    bool contains(const std::string& value) const
    {
        return choices.empty() || std::find(choices.begin(), choices.end(), value) != choices.end();
    }
};

template <class Value>
class TypedParameter : public Parameter
{
public:
    explicit TypedParameter(std::string name, Value defaultValue = {},
                            ParameterRange<Value> range = {}, bool required = false)
        : Parameter(std::move(name), required),
          default_(std::move(defaultValue)),
          value_(default_),
          range_(std::move(range))
    {}
    const Value& value() const { return value_; }
    const ParameterRange<Value>& range() const { return range_; }

private:
    bool parseValue(std::string_view text) override
    {
        Value candidate{};
        if (!internal::parseParameterValue(text, candidate) || !range_.contains(candidate))
            return false;

        value_ = std::move(candidate);
        return true;
    }
    void resetValue() override { value_ = default_; }
    Value default_;
    Value value_;
    ParameterRange<Value> range_;
};

template <class Value>
class ArrayParameter : public Parameter
{
public:
    explicit ArrayParameter(std::string name, std::vector<Value> defaults = {},
                            ParameterRange<Value> range = {}, size_t maxValues = 64,
                            bool required = false)
        : Parameter(std::move(name), required),
          defaults_(defaults.begin(), defaults.end()),
          values_(std::move(defaults)),
          range_(std::move(range)),
          maxValues_(maxValues)
    {}
    const std::vector<Value>& values() const { return values_; }
    bool empty() const { return values_.empty(); }
    size_t size() const { return values_.size(); }

private:
    bool parseValue(std::string_view text) override
    {
        std::vector<Value> candidate;
        for (;;) {
            const size_t comma = text.find(',');
            const auto token = text.substr(0, comma);
            Value value{};
            if (token.empty() || candidate.size() == maxValues_ ||
                !internal::parseParameterValue(token, value) || !range_.contains(value))
                return false;

            candidate.push_back(std::move(value));
            if (comma == std::string_view::npos)
                break;

            text.remove_prefix(comma + 1);
        }
        values_ = std::move(candidate);
        return true;
    }
    void resetValue() override { values_ = defaults_; }
    std::vector<Value> defaults_;
    std::vector<Value> values_;
    ParameterRange<Value> range_;
    size_t maxValues_;
};

using StringParameter = TypedParameter<std::string>;
using IntParameter = TypedParameter<int>;
using IntegerParameter = IntParameter;
using FloatParameter = TypedParameter<float>;
using DoubleParameter = TypedParameter<double>;
using SizeParameter = TypedParameter<size_t>;
using StringArrayParameter = ArrayParameter<std::string>;
using IntArrayParameter = ArrayParameter<int>;
using FloatArrayParameter = ArrayParameter<float>;
using SizeArrayParameter = ArrayParameter<size_t>;

class BoolParameter : public TypedParameter<bool>
{
public:
    explicit BoolParameter(std::string name, bool defaultValue = false, bool required = false)
        : TypedParameter(std::move(name), defaultValue, {}, required)
    {}
};
using BooleanParameter = BoolParameter;

}  // namespace http