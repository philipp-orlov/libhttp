// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "json/json.hpp"
#include "json/utf8.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

// std::to_chars/from_chars for double arrived in libstdc++ 11; the feature
// macro is the portable way to ask. Without them the number paths fall back to
// the iostreams formatting, which allocates -- so the integer fast path below
// matters most there.
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
#define JSON_FP_CHARCONV 1
#else
#define JSON_FP_CHARCONV 0
#endif

// GCC's -Wmaybe-uninitialized has a known false-positive pattern with
// std::variant when one alternative is a container of the enclosing type
// itself (here, Json holding vector<Json>/vector<pair<string,Json>>): it
// flags the variant's internal move/copy machinery as touching
// uninitialized storage, even though every access is guarded by the
// variant's own active-index check. Scoped to this translation unit only.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif

namespace json {

namespace {

// ---------------------------------------------------------- buffer pool ----
//
// Building and tearing down the same document shape millions of times is what
// a long-lived service does to this library, and each round would otherwise
// hand malloc a fresh vector that has to be grown 15 -> 30 -> 60 -> ... all
// over again. Retiring those buffers to a per-thread free list instead keeps
// the same blocks in rotation, so the heap settles at a fixed set of sizes.
//
// One document holds dozens of containers at once, so the free list has to be
// deep enough to hold them all or most of a round still goes to malloc. What
// bounds it is the memory it holds, not how many buffers that is: a single
// outlier document must not pin memory for the life of the process, and
// Json::trimCache releases whatever is held.
constexpr size_t kPoolBudget = 512 * 1024;  // bytes held per thread, all kinds together
constexpr size_t kPoolSlots = 256;          // buffers of each kind
constexpr size_t kMinPooledString = 16;     // shorter ones live inside the string itself
constexpr size_t kMaxPooledString = 4096;
constexpr size_t kMaxPooledArray = 256;  // elements, not bytes
constexpr size_t kMaxPooledObject = 128;

// Set once this thread's pool has been destroyed, so a Json torn down later in
// the same shutdown does not resurrect it.
thread_local bool gPoolRetired = false;

size_t footprint(const std::string& text)
{
    return text.capacity();
}
size_t footprint(const Json::Array& items)
{
    return items.capacity() * sizeof(Json);
}
size_t footprint(const Json::Object& fields)
{
    return fields.capacity() * sizeof(Json::Object::value_type);
}

struct Pool
{
    std::vector<std::string> strings;
    std::vector<Json::Array> arrays;
    std::vector<Json::Object> objects;
    size_t bytes = 0;

    // The lists themselves grow on demand -- a thread that parses one small
    // document at startup should not pay for a full-depth pool.
    //
    // The pooled containers are empty, so tearing the pool down cannot recurse
    // back into it; the flag guards values destroyed later in this thread's
    // shutdown.
    ~Pool() { gPoolRetired = true; }

    template <typename Buffer>
    bool admits(const std::vector<Buffer>& list, const Buffer& buffer) const
    {
        return list.size() < kPoolSlots && bytes + footprint(buffer) <= kPoolBudget;
    }
};

Pool* pool()
{
    if (gPoolRetired)
        return nullptr;

    static thread_local Pool instance;
    return &instance;
}

std::string takeString()
{
    Pool* buffers = pool();
    if (!buffers || buffers->strings.empty())
        return std::string();

    std::string recycled = std::move(buffers->strings.back());
    buffers->strings.pop_back();
    buffers->bytes -= footprint(recycled);
    recycled.clear();
    return recycled;
}

void giveString(std::string&& text) noexcept
{
    try {
        Pool* buffers = pool();
        if (!buffers || text.capacity() < kMinPooledString || text.capacity() > kMaxPooledString ||
            !buffers->admits(buffers->strings, text))
            return;

        text.clear();
        buffers->bytes += footprint(text);
        buffers->strings.push_back(std::move(text));
    } catch (...) {}
}

Json::Array takeArray()
{
    Pool* buffers = pool();
    if (!buffers || buffers->arrays.empty())
        return Json::Array();

    Json::Array recycled = std::move(buffers->arrays.back());
    buffers->arrays.pop_back();
    buffers->bytes -= footprint(recycled);
    return recycled;
}

void giveArray(Json::Array&& items) noexcept
{
    try {
        items.clear();  // each element retires its own buffers in turn
        Pool* buffers = pool();
        if (!buffers || items.capacity() == 0 || items.capacity() > kMaxPooledArray ||
            !buffers->admits(buffers->arrays, items))
            return;

        buffers->bytes += footprint(items);
        buffers->arrays.push_back(std::move(items));
    } catch (...) {}
}

Json::Object takeObject()
{
    Pool* buffers = pool();
    if (!buffers || buffers->objects.empty())
        return Json::Object();

    Json::Object recycled = std::move(buffers->objects.back());
    buffers->objects.pop_back();
    buffers->bytes -= footprint(recycled);
    return recycled;
}

void giveObject(Json::Object&& fields) noexcept
{
    try {
        for (auto& field : fields)
            giveString(std::move(field.first));

        fields.clear();
        Pool* buffers = pool();
        if (!buffers || fields.capacity() == 0 || fields.capacity() > kMaxPooledObject ||
            !buffers->admits(buffers->objects, fields))
            return;

        buffers->bytes += footprint(fields);
        buffers->objects.push_back(std::move(fields));
    } catch (...) {}
}

// -------------------------------------------------------------- writing ----

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

void appendUtf8(std::string& out, uint32_t codepoint)
{
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

// The two passes -- one that counts bytes, one that writes them -- share a
// single implementation through the sink type, so measuring costs nothing at
// runtime beyond the arithmetic and neither copy can drift from the other.
struct Counter
{
    size_t bytes = 0;

    void put(char) { ++bytes; }
    void put(std::string_view text) { bytes += text.size(); }
    void put(const char*, size_t length) { bytes += length; }
};

struct Appender
{
    std::string& out;

    void put(char c) { out.push_back(c); }
    void put(std::string_view text) { out.append(text.data(), text.size()); }
    void put(const char* text, size_t length) { out.append(text, length); }
};

template <typename Sink>
void putEscaped(Sink& sink, std::string_view text)
{
    sink.put('"');
    // Everything needing no escape goes out in runs; only the rare byte is
    // handled one at a time.
    size_t run = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 0x20 && c != '"' && c != '\\')
            continue;

        if (i > run)
            sink.put(text.substr(run, i - run));

        switch (c) {
            case '"': sink.put("\\\"", 2); break;
            case '\\': sink.put("\\\\", 2); break;
            case '\b': sink.put("\\b", 2); break;
            case '\f': sink.put("\\f", 2); break;
            case '\n': sink.put("\\n", 2); break;
            case '\r': sink.put("\\r", 2); break;
            case '\t': sink.put("\\t", 2); break;
            default: {
                static const char kHex[] = "0123456789abcdef";
                const char escape[6] = {'\\', 'u', '0', '0', kHex[c >> 4], kHex[c & 0x0F]};
                sink.put(escape, sizeof escape);
                break;
            }
        }
        run = i + 1;
    }
    if (text.size() > run)
        sink.put(text.substr(run));

    sink.put('"');
}

template <typename Sink>
void putNumber(Sink& sink, double value)
{
    if (!std::isfinite(value)) {
        sink.put("null", 4);
        return;
    }
    char buffer[40];
    // Counts, sizes and identifiers -- nearly everything a response carries --
    // are whole numbers, and the integer formatter is several times faster than
    // the shortest-round-trip one while producing the same digits. Negative
    // zero is left to the general path, which is the only one that keeps it.
    if (value >= -9007199254740992.0 && value <= 9007199254740992.0 &&
        !(value == 0 && std::signbit(value))) {
        const long long integral = static_cast<long long>(value);
        if (static_cast<double>(integral) == value) {
            const auto formatted = std::to_chars(buffer, buffer + sizeof buffer, integral);
            sink.put(buffer, static_cast<size_t>(formatted.ptr - buffer));
            return;
        }
    }
#if JSON_FP_CHARCONV
    const auto formatted = std::to_chars(buffer, buffer + sizeof buffer, value);
    sink.put(buffer, static_cast<size_t>(formatted.ptr - buffer));
#else
    std::ostringstream formatted;
    formatted.imbue(std::locale::classic());
    formatted << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    const std::string text = formatted.str();
    sink.put(text);
#endif
}

template <typename Sink>
void emit(Sink& sink, const Json& value)
{
    switch (value.type()) {
        case Json::Type::Null: sink.put("null", 4); break;
        case Json::Type::Bool:
            if (value.asBool())
                sink.put("true", 4);
            else
                sink.put("false", 5);

            break;
        case Json::Type::Number:
            if (value.isInteger()) {
                char digits[24];
                const auto formatted =
                    std::to_chars(digits, digits + sizeof digits, value.asInt());
                sink.put(digits, static_cast<size_t>(formatted.ptr - digits));
            } else {
                putNumber(sink, value.asDouble());
            }
            break;
        case Json::Type::String: putEscaped(sink, value.asString()); break;
        case Json::Type::Array: {
            sink.put('[');
            const Json::Array& items = value.asArray();
            for (size_t i = 0; i < items.size(); ++i) {
                if (i)
                    sink.put(',');

                emit(sink, items[i]);
            }
            sink.put(']');
            break;
        }
        case Json::Type::Object: {
            sink.put('{');
            const Json::Object& fields = value.asObject();
            for (size_t i = 0; i < fields.size(); ++i) {
                if (i)
                    sink.put(',');

                putEscaped(sink, fields[i].first);
                sink.put(':');
                emit(sink, fields[i].second);
            }
            sink.put('}');
            break;
        }
    }
}

// Recursive-descent JSON parser. One-shot over an already fully-buffered
// string (an HTTP request body never streams in incrementally by the time
// a handler sees it), so there's no need for the resumable-across-reads
// machinery HttpParser needs.
class Parser
{
public:
    explicit Parser(std::string_view text) : text_(text) {}

    bool parseDocument(Json& out, std::string& error)
    {
        if (text_.size() > 16 * 1024 * 1024)
            return fail(error, "JSON document exceeds 16 MiB");

        skipWhitespace();
        if (!parseValue(out, error))
            return false;

        skipWhitespace();
        if (pos_ != text_.size())
            return fail(error, "trailing content after JSON value");

        return true;
    }

private:
    std::string_view text_;
    size_t pos_ = 0;
    size_t depth_ = 0;
    size_t nodes_ = 0;

    bool fail(std::string& error, std::string_view message)
    {
        size_t line = 1, col = 1;
        for (size_t i = 0; i < pos_ && i < text_.size(); ++i) {
            if (text_[i] == '\n') {
                ++line;
                col = 1;
            } else {
                ++col;
            }
        }
        error = "JSON parse error at line " + std::to_string(line) + ", column " +
                std::to_string(col) + ": " + std::string(message);
        return false;
    }

    void skipWhitespace()
    {
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++pos_;
            else
                break;
        }
    }

    char peek() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

    bool parseValue(Json& out, std::string& error)
    {
        if (depth_ == 64 || ++nodes_ > 100000)
            return fail(error, "JSON structure limit exceeded");

        struct DepthGuard
        {
            size_t& depth;
            explicit DepthGuard(size_t& value) : depth(value) { ++depth; }
            ~DepthGuard() { --depth; }
        } guard(depth_);
        skipWhitespace();
        if (pos_ >= text_.size())
            return fail(error, "unexpected end of input");

        char c = text_[pos_];
        switch (c) {
            case '{': return parseObject(out, error);
            case '[': return parseArray(out, error);
            case '"': {
                // Parsed straight into the value's own (recycled) buffer, so
                // no temporary string is built and thrown away per key/value.
                out = Json(takeString());
                return parseString(out.asString(), error);
            }
            case 't': return parseLiteral("true", Json(true), out, error);
            case 'f': return parseLiteral("false", Json(false), out, error);
            case 'n': return parseLiteral("null", Json(nullptr), out, error);
            default:
                if (c == '-' || isDigit(c))
                    return parseNumber(out, error);

                return fail(error, "unexpected character");
        }
    }

    bool parseLiteral(std::string_view literal, Json value, Json& out, std::string& error)
    {
        if (text_.substr(pos_, literal.size()) != literal)
            return fail(error, "invalid literal");

        pos_ += literal.size();
        out = std::move(value);
        return true;
    }

    bool parseNumber(Json& out, std::string& error)
    {
        size_t start = pos_;
        bool fractionOrExponent = false;
        if (peek() == '-')
            ++pos_;

        if (pos_ >= text_.size() || !isDigit(text_[pos_]))
            return fail(error, "invalid number");

        if (text_[pos_] == '0') {
            ++pos_;
        } else {
            while (pos_ < text_.size() && isDigit(text_[pos_]))
                ++pos_;
        }
        if (pos_ < text_.size() && text_[pos_] == '.') {
            fractionOrExponent = true;
            ++pos_;
            if (pos_ >= text_.size() || !isDigit(text_[pos_]))
                return fail(error, "invalid number");

            while (pos_ < text_.size() && isDigit(text_[pos_]))
                ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            fractionOrExponent = true;
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-'))
                ++pos_;

            if (pos_ >= text_.size() || !isDigit(text_[pos_]))
                return fail(error, "invalid number");

            while (pos_ < text_.size() && isDigit(text_[pos_]))
                ++pos_;
        }
        if (pos_ - start > 128)
            return fail(error, "number token too long");

        // An integer token (no fraction, no exponent) that fits keeps every digit.
        // "-0" stays a double so its sign survives.
        if (!fractionOrExponent && !(pos_ - start == 2 && text_[start] == '-' && text_[start + 1] == '0')) {
            long long integer = 0;
            const char* begin = text_.data() + start;
            const char* end = text_.data() + pos_;
            const auto parsedInteger = std::from_chars(begin, end, integer);
            if (parsedInteger.ec == std::errc() && parsedInteger.ptr == end) {
                out = Json(integer);
                return true;
            }
        }
        double value = 0;
#if JSON_FP_CHARCONV
        // The scanner above has already proved the token is a JSON number, so
        // from_chars cannot wander into "inf"/"nan"; it is used here mainly
        // because it allocates nothing, unlike the stringstream it replaced.
        const char* first = text_.data() + start;
        const char* last = text_.data() + pos_;
        const auto parsed = std::from_chars(first, last, value);
        if (parsed.ec != std::errc() || parsed.ptr != last || !std::isfinite(value))
            return fail(error, "number outside finite range");
#else
        std::istringstream token(std::string(text_.substr(start, pos_ - start)));
        token.imbue(std::locale::classic());
        token >> value;
        if (token.fail() || !token.eof() || !std::isfinite(value))
            return fail(error, "number outside finite range");
#endif
        out = Json(value);
        return true;
    }

    bool parseHex4(uint32_t& out, std::string& error)
    {
        if (pos_ + 4 > text_.size())
            return fail(error, "truncated \\u escape");

        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            char c = text_[pos_ + i];
            value <<= 4;
            if (c >= '0' && c <= '9')
                value |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                value |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                value |= static_cast<uint32_t>(c - 'A' + 10);
            else
                return fail(error, "invalid hex digit in \\u escape");
        }
        pos_ += 4;
        out = value;
        return true;
    }

    bool parseString(std::string& out, std::string& error)
    {
        ++pos_;  // opening quote
        out.clear();
        // Unescaped stretches -- which is almost all of any real payload -- are
        // copied in one append instead of a character at a time.
        size_t run = pos_;
        auto flush = [&] {
            if (pos_ > run)
                out.append(text_.data() + run, pos_ - run);
        };
        while (true) {
            if (pos_ >= text_.size())
                return fail(error, "unterminated string");

            char c = text_[pos_];
            if (c == '"') {
                flush();
                ++pos_;
                return isValidUtf8(out) || fail(error, "invalid UTF-8 string");
            }
            if (static_cast<unsigned char>(c) < 0x20)
                return fail(error, "control character in string");

            if (c != '\\') {
                ++pos_;
                continue;
            }

            flush();
            ++pos_;
            if (pos_ >= text_.size())
                return fail(error, "unterminated escape");

            char e = text_[pos_];
            switch (e) {
                case '"':
                    out.push_back('"');
                    ++pos_;
                    break;
                case '\\':
                    out.push_back('\\');
                    ++pos_;
                    break;
                case '/':
                    out.push_back('/');
                    ++pos_;
                    break;
                case 'b':
                    out.push_back('\b');
                    ++pos_;
                    break;
                case 'f':
                    out.push_back('\f');
                    ++pos_;
                    break;
                case 'n':
                    out.push_back('\n');
                    ++pos_;
                    break;
                case 'r':
                    out.push_back('\r');
                    ++pos_;
                    break;
                case 't':
                    out.push_back('\t');
                    ++pos_;
                    break;
                case 'u': {
                    ++pos_;
                    uint32_t codepoint = 0;
                    if (!parseHex4(codepoint, error))
                        return false;

                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                        if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' ||
                            text_[pos_ + 1] != 'u') {
                            return fail(error, "unpaired surrogate in \\u escape");
                        }
                        pos_ += 2;
                        uint32_t low = 0;
                        if (!parseHex4(low, error))
                            return false;

                        if (low < 0xDC00 || low > 0xDFFF)
                            return fail(error, "invalid low surrogate");

                        codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                    } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                        return fail(error, "unpaired low surrogate");
                    }
                    appendUtf8(out, codepoint);
                    break;
                }
                default: return fail(error, "invalid escape sequence");
            }
            run = pos_;
        }
    }

    bool parseArray(Json& out, std::string& error)
    {
        ++pos_;  // '['
        // Held in a Json from the start, so the vector comes from the pool and
        // goes back to it even when the document turns out to be malformed.
        Json result = Json::array();
        Json::Array& items = result.asArray();
        skipWhitespace();
        if (peek() == ']') {
            ++pos_;
            out = std::move(result);
            return true;
        }
        while (true) {
            items.emplace_back();
            if (!parseValue(items.back(), error))
                return false;

            skipWhitespace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == ']') {
                ++pos_;
                break;
            }
            return fail(error, "expected ',' or ']'");
        }
        out = std::move(result);
        return true;
    }

    bool parseObject(Json& out, std::string& error)
    {
        ++pos_;  // '{'
        Json result = Json::object();
        Json::Object& fields = result.asObject();
        // Open-addressing index of the keys (slot = field index + 1, 0 = empty),
        // used only once the object is large; hashes[i] belongs to fields[i].
        constexpr size_t kIndexAfter = 32;
        std::vector<uint32_t> slots;
        std::vector<size_t> hashes;
        const auto place = [](std::vector<uint32_t>& table, size_t hash, size_t index) {
            size_t probe = hash & (table.size() - 1);
            while (table[probe])
                probe = (probe + 1) & (table.size() - 1);

            table[probe] = static_cast<uint32_t>(index + 1);
        };
        skipWhitespace();
        if (peek() == '}') {
            ++pos_;
            out = std::move(result);
            return true;
        }
        while (true) {
            skipWhitespace();
            if (peek() != '"')
                return fail(error, "expected string key");

            std::string key = takeString();
            if (!parseString(key, error))
                return false;

            if (fields.size() == 4096)
                return fail(error, "too many object members");

            // Small objects: a scan. Larger ones: a hash index built when the
            // object passes kIndexAfter members, so 4096 keys cost 4096 probes
            // rather than 8 million string compares.
            const size_t hash = std::hash<std::string_view>{}(key);
            if (fields.size() < kIndexAfter) {
                for (const auto& field : fields)
                    if (field.first == key)
                        return fail(error, "duplicate object key");
            } else {
                if (slots.empty()) {
                    slots.assign(256, 0);
                    hashes.clear();
                    for (const auto& field : fields) {
                        hashes.push_back(std::hash<std::string_view>{}(field.first));
                        place(slots, hashes.back(), hashes.size() - 1);
                    }
                }
                for (size_t probe = hash & (slots.size() - 1); slots[probe];
                     probe = (probe + 1) & (slots.size() - 1)) {
                    const size_t candidate = slots[probe] - 1;
                    if (hashes[candidate] == hash && fields[candidate].first == key)
                        return fail(error, "duplicate object key");
                }
            }
            skipWhitespace();
            if (peek() != ':')
                return fail(error, "expected ':'");

            ++pos_;
            fields.emplace_back(std::move(key), Json());
            if (!slots.empty()) {
                hashes.push_back(hash);
                if (hashes.size() * 2 > slots.size()) {
                    slots.assign(slots.size() * 2, 0);
                    for (size_t index = 0; index < hashes.size(); ++index)
                        place(slots, hashes[index], index);
                } else {
                    place(slots, hash, hashes.size() - 1);
                }
            }
            if (!parseValue(fields.back().second, error))
                return false;

            skipWhitespace();
            if (peek() == ',') {
                ++pos_;
                continue;
            }
            if (peek() == '}') {
                ++pos_;
                break;
            }
            return fail(error, "expected ',' or '}'");
        }
        out = std::move(result);
        return true;
    }
};

}  // namespace

Json::~Json()
{
    recycle();
}

Json::Json(std::initializer_list<Field> fields)
{
    value_ = takeObject();
    Object& object = std::get<Object>(value_);
    object.reserve(fields.size());
    for (const Field& field : fields) {
        std::string key = takeString();
        key.assign(field.key);
        object.emplace_back(std::move(key), std::move(field.value));
    }
}

Json& Json::operator=(Json&& other) noexcept
{
    if (this != &other) {
        // Retired first: a move-assignment steals the source's buffer, so the
        // one being overwritten would otherwise go straight back to malloc.
        recycle();
        value_ = std::move(other.value_);
    }
    return *this;
}

void Json::recycle() noexcept
{
    if (auto* text = std::get_if<std::string>(&value_))
        giveString(std::move(*text));
    else if (auto* items = std::get_if<Array>(&value_))
        giveArray(std::move(*items));
    else if (auto* fields = std::get_if<Object>(&value_))
        giveObject(std::move(*fields));
}

Json Json::array()
{
    Json value;
    value.value_ = takeArray();
    return value;
}

Json Json::object()
{
    Json value;
    value.value_ = takeObject();
    return value;
}

void Json::trimCache() noexcept
{
    Pool* buffers = pool();
    if (!buffers)
        return;

    buffers->strings.clear();
    buffers->arrays.clear();
    buffers->objects.clear();
    buffers->bytes = 0;
}

bool Json::parse(std::string_view text, Json& out, std::string& error)
{
    Parser parser(text);
    return parser.parseDocument(out, error);
}

bool Json::tryInt64(long long& out) const noexcept
{
    if (const long long* integer = std::get_if<long long>(&value_)) {
        out = *integer;
        return true;
    }
    if (const double* number = std::get_if<double>(&value_)) {
        // The bounds are exactly -2^63 and 2^63, both representable as doubles.
        if (std::isfinite(*number) && *number >= -9223372036854775808.0 &&
            *number < 9223372036854775808.0 && std::trunc(*number) == *number) {
            out = static_cast<long long>(*number);
            return true;
        }
    }
    return false;
}

long long Json::asInt() const
{
    if (const long long* integer = std::get_if<long long>(&value_))
        return *integer;

    const double number = std::get<double>(value_);
    if (!(number >= -9223372036854775808.0 && number < 9223372036854775808.0))
        throw std::range_error("JSON number does not fit in a 64-bit integer");

    return static_cast<long long>(number);
}

long long Json::asIntChecked(long long lo, long long hi) const
{
    long long value = 0;
    if (!tryInt64(value))
        throw std::range_error("JSON number is not an integer");

    if (value < lo || value > hi)
        throw std::range_error("JSON integer out of range");

    return value;
}

void Json::push_back(Json value)
{
    if (isNull())
        value_ = takeArray();
    asArray().push_back(std::move(value));
}

const Json* Json::find(std::string_view key) const
{
    if (!isObject())
        return nullptr;

    for (auto& field : asObject()) {
        if (field.first == key)
            return &field.second;
    }
    return nullptr;
}

Json& Json::operator[](std::string_view key)
{
    if (isNull())
        value_ = takeObject();
    Object& fields = asObject();
    for (auto& field : fields) {
        if (field.first == key)
            return field.second;
    }
    std::string name = takeString();
    name.assign(key);
    fields.emplace_back(std::move(name), Json());
    return fields.back().second;
}

size_t Json::measure() const
{
    Counter counter;
    emit(counter, *this);
    return counter.bytes;
}

void Json::dump(std::string& out) const
{
    // One growth per call at most, to exactly the size the document needs, and
    // none at all once the caller's buffer has held one this big before.
    const size_t needed = measure();
    if (out.capacity() - out.size() < needed)
        out.reserve(out.size() + needed);

    Appender appender{out};
    emit(appender, *this);
}

std::string Json::dump() const
{
    std::string out;
    dump(out);
    return out;
}

}  // namespace json

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
