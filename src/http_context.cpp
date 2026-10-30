// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/http_context.hpp"

#include "http/http_parser.hpp"

namespace http {

namespace {
// Spare entries kept for reuse, as in HeaderList.
constexpr size_t kMaxSpare = 32;

// A new last entry of `items`, made from a spare one (strings and their
// storage intact) when there is one.
std::pair<std::string, std::string>& addEntry(
    std::vector<std::pair<std::string, std::string>>& items,
    std::vector<std::pair<std::string, std::string>>& spare)
{
    if (spare.empty()) {
        items.emplace_back();
    } else {
        items.push_back(std::move(spare.back()));
        spare.pop_back();
    }
    return items.back();
}

void retireEntries(std::vector<std::pair<std::string, std::string>>& items,
                   std::vector<std::pair<std::string, std::string>>& spare)
{
    for (auto& item : items) {
        if (spare.size() < kMaxSpare)
            spare.push_back(std::move(item));
    }
    items.clear();
}
}  // namespace

void HttpContext::reset()
{
    queryParsed_ = false;
    retireEntries(queryParams_, spare_);
    cookiesParsed_ = false;
    retireEntries(cookies_, spare_);
    routeParams_.clear();
}

size_t HttpContext::trimSpare(size_t maxBytes)
{
    size_t kept = 0;
    for (auto& entry : spare_) {
        if (kept + entry.first.capacity() + entry.second.capacity() > maxBytes) {
            std::string().swap(entry.first);
            std::string().swap(entry.second);
        }
        kept += entry.first.capacity() + entry.second.capacity();
    }
    if (remoteAddr_.capacity() > maxBytes)
        std::string().swap(remoteAddr_);

    return kept + remoteAddr_.capacity();
}

void HttpContext::ensureQueryParsed() const
{
    if (queryParsed_)
        return;

    queryParsed_ = true;
    std::string_view qs = request_.queryString;
    size_t pos = 0;
    while (pos < qs.size()) {
        size_t amp = qs.find('&', pos);
        std::string_view pair =
            qs.substr(pos, amp == std::string_view::npos ? std::string_view::npos : amp - pos);
        if (!pair.empty()) {
            size_t eq = pair.find('=');
            auto& entry = addEntry(queryParams_, spare_);
            urlDecode(pair.substr(0, eq), /*plusAsSpace=*/true, entry.first);
            if (eq == std::string_view::npos)
                entry.second.clear();
            else
                urlDecode(pair.substr(eq + 1), /*plusAsSpace=*/true, entry.second);
        }
        if (amp == std::string_view::npos)
            break;

        pos = amp + 1;
    }
}

void HttpContext::ensureCookiesParsed() const
{
    if (cookiesParsed_)
        return;

    cookiesParsed_ = true;
    std::string_view header = request_.headers.get("Cookie");
    size_t pos = 0;
    while (pos < header.size()) {
        size_t semi = header.find(';', pos);
        std::string_view pair = header.substr(
            pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos);
        while (!pair.empty() && pair.front() == ' ')
            pair.remove_prefix(1);
        size_t eq = pair.find('=');
        if (eq != std::string_view::npos) {
            auto& entry = addEntry(cookies_, spare_);
            entry.first.assign(pair.data(), eq);
            urlDecode(pair.substr(eq + 1), /*plusAsSpace=*/false, entry.second);
        }
        if (semi == std::string_view::npos)
            break;

        pos = semi + 1;
    }
}

std::string_view HttpContext::query(std::string_view name) const
{
    ensureQueryParsed();
    for (auto& kv : queryParams_) {
        if (kv.first == name)
            return kv.second;
    }
    return {};
}

bool HttpContext::hasQuery(std::string_view name) const
{
    ensureQueryParsed();
    for (auto& kv : queryParams_) {
        if (kv.first == name)
            return true;
    }
    return false;
}

const std::vector<std::pair<std::string, std::string>>& HttpContext::queryParams() const
{
    ensureQueryParsed();
    return queryParams_;
}

void HttpContext::parseQuery(std::initializer_list<Parameter*> parameters,
                             QueryParseOptions options) const
{
    for (auto* parameter : parameters)
        parameter->reset();

    for (const auto& entry : queryParams()) {
        Parameter* matched = nullptr;
        for (auto* parameter : parameters) {
            if (parameter->is(entry.first, options.caseInsensitiveNames)) {
                matched = parameter;
                break;
            }
        }
        if (!matched) {
            if (options.unknown == UnknownParameters::Reject)
                throw ParameterError(entry.first, "unsupported parameter");

            continue;
        }
        if (matched->parsed()) {
            if (options.duplicates == DuplicateParameters::Reject)
                throw ParameterError(entry.first, "duplicate parameter");

            if (options.duplicates == DuplicateParameters::First)
                continue;
        }
        matched->parse(entry.second);
    }
    for (const auto* parameter : parameters) {
        if (parameter->required() && !parameter->parsed())
            throw ParameterError(parameter->name(), "required parameter is missing");
    }
}

std::string_view HttpContext::cookie(std::string_view name) const
{
    ensureCookiesParsed();
    for (auto& kv : cookies_) {
        if (kv.first == name)
            return kv.second;
    }
    return {};
}

bool HttpContext::hasCookie(std::string_view name) const
{
    ensureCookiesParsed();
    for (auto& kv : cookies_) {
        if (kv.first == name)
            return true;
    }
    return false;
}

std::string_view HttpContext::routeParam(std::string_view name) const
{
    for (auto& kv : routeParams_) {
        if (kv.first == name)
            return kv.second;
    }
    return {};
}

namespace {
// RFC 6265 cookie-octet: no controls, space, '"', ',', ';' or '\\'.
bool isCookieValue(std::string_view value)
{
    for (const char c : value) {
        const unsigned char v = static_cast<unsigned char>(c);
        if (v <= 32 || v >= 127 || v == '"' || v == ',' || v == ';' || v == '\\')
            return false;
    }
    return true;
}

// A Path or Domain attribute value: printable, and no ';' to end it early.
bool isCookieAttribute(std::string_view value)
{
    for (const char c : value) {
        const unsigned char v = static_cast<unsigned char>(c);
        if (v < 32 || v >= 127 || v == ';')
            return false;
    }
    return true;
}
}  // namespace

HttpContext& HttpContext::setCookie(const CookieOptions& options)
{
    if (!isHeaderName(options.name) || !isCookieValue(options.value) ||
        !isCookieAttribute(options.path) || !isCookieAttribute(options.domain))
        throw std::invalid_argument("invalid cookie");

    if (!options.sameSite.empty() && !equalsIgnoreCase(options.sameSite, "Strict") &&
        !equalsIgnoreCase(options.sameSite, "Lax") && !equalsIgnoreCase(options.sameSite, "None"))
        throw std::invalid_argument("invalid cookie SameSite");

    std::string value = options.name + "=" + options.value;
    value += "; Path=" + (options.path.empty() ? std::string("/") : options.path);
    if (!options.domain.empty())
        value += "; Domain=" + options.domain;

    if (options.maxAgeSeconds >= 0)
        value += "; Max-Age=" + std::to_string(options.maxAgeSeconds);

    if (options.secure)
        value += "; Secure";

    if (options.httpOnly)
        value += "; HttpOnly";

    if (!options.sameSite.empty())
        value += "; SameSite=" + options.sameSite;

    response_.headers.add("Set-Cookie", value);
    return *this;
}

HttpContext& HttpContext::text(std::string_view body, std::string_view contentType)
{
    response_.setHeader("Content-Type", contentType);
    response_.body.assign(body.data(), body.size());
    if (response_.pooledBody.capacity())
        response_.pooledBody = Buffer();

    return *this;
}

HttpContext& HttpContext::json(const json::Json& body)
{
    response_.setHeader("Content-Type", "application/json; charset=utf-8");
    // Rendered straight into the response buffer: no temporary document string
    // is built and thrown away per request.
    response_.body.clear();
    body.dump(response_.body);
    if (response_.pooledBody.capacity())
        response_.pooledBody = Buffer();

    return *this;
}

HttpContext& HttpContext::bytes(const void* data, size_t len, std::string_view contentType)
{
    response_.setHeader("Content-Type", contentType);
    response_.body.assign(static_cast<const char*>(data), len);
    if (response_.pooledBody.capacity())
        response_.pooledBody = Buffer();

    return *this;
}

HttpContext& HttpContext::bytes(Buffer&& body, std::string_view contentType)
{
    response_.setHeader("Content-Type", contentType);
    response_.body.clear();
    response_.pooledBody = std::move(body);
    return *this;
}

}  // namespace http
