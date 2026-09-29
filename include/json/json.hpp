// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace json {

class Json;

namespace detail {

// The element of an object literal. Elements of an initializer_list are const,
// so the value is held behind `mutable` and moved out of the list -- without
// that, `Json{{"items", std::move(big)}}` would deep-copy everything it was
// handed. The key stays a view (every temporary in the literal outlives the
// Json being built from it) so that it can be copied into a recycled buffer
// instead of a freshly allocated one. It is a template, and lives out here,
// only because Json is still an incomplete type where its literal constructor
// is declared.
template <typename Value>
struct Field
{
    template <typename T>
    Field(std::string_view name, T&& value) : key(name), value(std::forward<T>(value))
    {}

    std::string_view key;
    mutable Value value;
};

}  // namespace detail

// A small, self-contained JSON value -- no 3rd-party dependency. Build one
// fluently for an HTTP response:
//   Json body{{"status", "ok"}, {"visits", 3}};
//   Json list = Json::array();
//   list.push_back(Json{{"name", "q"}, {"value", "widgets"}});
// or parse one from text:
//   Json value; std::string error;
//   if (Json::parse(request.body.view(), value, error)) { ... }
//
// Numbers are held as double (as JSON itself defines only one numeric
// type) -- except that an integer with no fraction or exponent, whether
// parsed or built from an integer type, is kept as a 64-bit integer so
// identifiers above 2^53 survive a round trip exactly. Both are
// Type::Number; asDouble() converts, and asInt()/tryInt64()/asIntChecked()
// read the integer, refusing (instead of the undefined double -> integer
// conversion) a value that is not integral or does not fit. Objects and
// arrays preserve insertion/parse order -- a flat vector<pair>, matching the
// header-list style already used elsewhere in this codebase -- rather than
// sorting by key.
class Json
{
public:
    enum class Type
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };
    using Array = std::vector<Json>;
    using Object = std::vector<std::pair<std::string, Json>>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool value) : value_(value) {}
    Json(int value) : value_(static_cast<long long>(value)) {}
    Json(unsigned value) : value_(static_cast<long long>(value)) {}
    Json(long value) : value_(static_cast<long long>(value)) {}
    Json(unsigned long value) { assignUnsigned(value); }
    Json(long long value) : value_(value) {}
    Json(unsigned long long value) { assignUnsigned(value); }
    Json(double value) : value_(value) {}
    Json(const char* value) : value_(std::string(value)) {}
    Json(std::string value) : value_(std::move(value)) {}
    Json(std::string_view value) : value_(std::string(value)) {}
    Json(Array value) : value_(std::move(value)) {}
    Json(Object value) : value_(std::move(value)) {}

    // Object literal syntax: Json{{"key", value}, {"other", 42}}.
    using Field = detail::Field<Json>;
    Json(std::initializer_list<Field> fields);

    // A string/array/object buffer is handed to a small per-thread free list
    // when the value holding it dies, and taken back from it when the next one
    // is built. A process that answers the same shaped request for months then
    // reuses the same blocks -- already grown to the right size -- instead of
    // churning the heap into fragments.
    ~Json();
    Json(const Json&) = default;
    Json& operator=(const Json&) = default;
    Json(Json&& other) noexcept : value_(std::move(other.value_)) {}
    Json& operator=(Json&& other) noexcept;

    static Json array();
    static Json object();

    // Releases this thread's recycled buffers, after an outlier document would
    // otherwise leave them held for the life of the process.
    static void trimCache() noexcept;

    // Parses `text` into `out`; returns false and fills `error` (with a
    // 1-based line/column) on malformed input. One-shot, not incremental --
    // JSON bodies are already fully buffered by the time a handler sees them.
    static bool parse(std::string_view text, Json& out, std::string& error);

    Type type() const
    {
        // The 64-bit integer alternative (last) is a number too.
        return value_.index() == kIntegerIndex ? Type::Number : static_cast<Type>(value_.index());
    }
    bool isNull() const { return type() == Type::Null; }
    bool isBool() const { return type() == Type::Bool; }
    bool isNumber() const { return type() == Type::Number; }
    bool isString() const { return type() == Type::String; }
    bool isArray() const { return type() == Type::Array; }
    bool isObject() const { return type() == Type::Object; }

    bool asBool() const { return std::get<bool>(value_); }
    double asDouble() const
    {
        if (value_.index() == kIntegerIndex)
            return static_cast<double>(std::get<long long>(value_));

        return std::get<double>(value_);
    }
    // The number as an integer. A fractional value is truncated toward zero;
    // throws std::range_error when it is NaN/infinite or does not fit in 64
    // bits (std::bad_variant_access when this is not a number at all).
    long long asInt() const;
    // True and the value when this is an integral number that fits in 64 bits.
    bool tryInt64(long long& out) const noexcept;
    // The integer, only if integral and lo <= value <= hi; std::range_error
    // otherwise. The accessor for values that came from a client.
    long long asIntChecked(long long lo, long long hi) const;
    // True when the number is held as a 64-bit integer rather than a double.
    bool isInteger() const { return value_.index() == kIntegerIndex; }
    const std::string& asString() const { return std::get<std::string>(value_); }
    std::string& asString() { return std::get<std::string>(value_); }
    const Array& asArray() const { return std::get<Array>(value_); }
    Array& asArray() { return std::get<Array>(value_); }
    const Object& asObject() const { return std::get<Object>(value_); }
    Object& asObject() { return std::get<Object>(value_); }

    // Appends to an array, turning a freshly-default-constructed (Null)
    // Json into an empty array first so `Json j; j.push_back(1);` works.
    void push_back(Json value);

    // Object field lookup by name; nullptr if absent or not an object.
    const Json* find(std::string_view key) const;

    // Mutable field access for building an object incrementally, turning a
    // Null Json into an empty object first: `Json j; j["a"] = 1;`.
    Json& operator[](std::string_view key);
    Json& operator[](size_t index) { return asArray()[index]; }
    const Json& operator[](size_t index) const { return asArray()[index]; }

    // Compact JSON text (no pretty-printing/indentation).
    std::string dump() const;

    // The same text appended onto a buffer the caller owns and recycles --
    // the form to use in a response loop. The document is measured before it
    // is written, so `out` grows at most once per call, to exactly what is
    // needed, and not at all once it has held a document of this size before.
    void dump(std::string& out) const;

    // How many bytes dump() would produce, without producing them.
    size_t measure() const;

private:
    static constexpr size_t kIntegerIndex = 6;

    void assignUnsigned(unsigned long long value)
    {
        if (value <= static_cast<unsigned long long>(std::numeric_limits<long long>::max()))
            value_ = static_cast<long long>(value);
        else
            value_ = static_cast<double>(value);
    }

    void recycle() noexcept;

    std::variant<std::monostate, bool, double, std::string, Array, Object, long long> value_;
};

}  // namespace json
