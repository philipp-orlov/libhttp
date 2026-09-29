// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/static_files.hpp"

#include <filesystem>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#endif

#include "http/logger.hpp"

namespace fs = std::filesystem;

namespace http {

struct StaticFileHandler::RootHandle
{
#ifndef _WIN32
    int descriptor = -1;
    explicit RootHandle(const std::string& path)
    {
        descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (descriptor < 0)
            throw std::runtime_error("cannot open static root");
    }
    ~RootHandle() { ::close(descriptor); }
#else
    explicit RootHandle(const std::string&) {}
#endif
};

std::string_view mimeTypeForExtension(std::string_view path)
{
    size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos)
        return "application/octet-stream";

    std::string_view ext = path.substr(dot + 1);

    static const std::pair<std::string_view, std::string_view> kTypes[] = {
        {"html", "text/html; charset=utf-8"},
        {"htm", "text/html; charset=utf-8"},
        {"css", "text/css; charset=utf-8"},
        {"js", "application/javascript; charset=utf-8"},
        {"mjs", "application/javascript; charset=utf-8"},
        {"json", "application/json; charset=utf-8"},
        {"txt", "text/plain; charset=utf-8"},
        {"xml", "application/xml; charset=utf-8"},
        {"png", "image/png"},
        {"jpg", "image/jpeg"},
        {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},
        {"webp", "image/webp"},
        {"bmp", "image/bmp"},
        {"svg", "image/svg+xml"},
        {"ico", "image/x-icon"},
        {"pdf", "application/pdf"},
        {"wasm", "application/wasm"},
        {"woff", "font/woff"},
        {"woff2", "font/woff2"},
        {"mp4", "video/mp4"},
        {"bin", "application/octet-stream"},
    };
    for (auto& [k, v] : kTypes) {
        if (k.size() == ext.size() &&
            std::equal(k.begin(), k.end(), ext.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            })) {
            return v;
        }
    }
    return "application/octet-stream";
}

namespace {

// Normalizes a decoded URL path into a safe relative filesystem path,
// resolving "." and ".." without ever escaping above the root. Returns
// false if the path tries to escape (e.g. leading "../").
bool normalizeRelativePath(std::string_view path, std::string& outRelative)
{
    if (path.find('\0') != std::string_view::npos || path.find('\\') != std::string_view::npos)
        return false;

    std::vector<std::string_view> stack;
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t slash = path.find('/', pos);
        std::string_view part = path.substr(
            pos, slash == std::string_view::npos ? std::string_view::npos : slash - pos);
        if (part.empty() || part == ".") {
            // skip
        } else if (part == "..") {
            if (stack.empty())
                return false;  // would escape root

            stack.pop_back();
        } else {
            stack.push_back(part);
        }
        if (slash == std::string_view::npos)
            break;

        pos = slash + 1;
    }
    std::ostringstream oss;
    for (size_t i = 0; i < stack.size(); ++i) {
        if (i > 0)
            oss << '/';

        oss << stack[i];
    }
    outRelative = oss.str();
    return true;
}

bool hasDotComponent(std::string_view relative)
{
    size_t start = 0;
    while (start < relative.size()) {
        if (relative[start] == '.')
            return true;

        const size_t slash = relative.find('/', start);
        if (slash == std::string_view::npos)
            break;

        start = slash + 1;
    }
    return false;
}

std::string lowerExtension(std::string_view relative)
{
    const size_t slash = relative.find_last_of('/');
    const std::string_view name =
        slash == std::string_view::npos ? relative : relative.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    std::string extension;
    if (dot != std::string_view::npos)
        for (const char c : name.substr(dot + 1))
            extension += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    return extension;
}

// A weak validator from the size and modification time, hashed so that the
// tag neither exposes the mtime nor an inode number.
std::string makeEtag(uint64_t size, int64_t seconds, int64_t nanoseconds)
{
    uint64_t hash = 1469598103934665603ull;
    for (const uint64_t part : {size, static_cast<uint64_t>(seconds),
                                static_cast<uint64_t>(nanoseconds)}) {
        for (int shift = 0; shift < 64; shift += 8) {
            hash ^= (part >> shift) & 0xff;
            hash *= 1099511628211ull;
        }
    }
    char text[32];
    std::snprintf(text, sizeof(text), "W/\"%016llx\"", static_cast<unsigned long long>(hash));
    return text;
}

// If-None-Match: "*" or a comma-separated list, compared weakly (RFC 9110 8.8.3.2).
bool etagMatches(std::string_view header, std::string_view etag)
{
    const auto opaque = [](std::string_view tag) {
        while (!tag.empty() && (tag.front() == ' ' || tag.front() == '\t'))
            tag.remove_prefix(1);
        while (!tag.empty() && (tag.back() == ' ' || tag.back() == '\t'))
            tag.remove_suffix(1);
        if (tag.substr(0, 2) == "W/")
            tag.remove_prefix(2);

        return tag;
    };
    const std::string_view wanted = opaque(etag);
    while (!header.empty()) {
        const size_t comma = header.find(',');
        const std::string_view candidate = opaque(header.substr(0, comma));
        if (candidate == "*" || candidate == wanted)
            return true;

        if (comma == std::string_view::npos)
            break;

        header.remove_prefix(comma + 1);
    }
    return false;
}

void applyFileHeaders(HttpContext& context, std::string_view relative,
                      const StaticFileHandler::Options& options)
{
    HttpResponse& response = context.response();
    response.setHeader("X-Content-Type-Options", "nosniff");
    if (!options.cacheControl.empty())
        response.setHeader("Cache-Control", options.cacheControl);

    // Same-origin active content: a served SVG must not run script.
    if (lowerExtension(relative) == "svg")
        response.setHeader("Content-Security-Policy",
                           "default-src 'none'; style-src 'unsafe-inline'; sandbox");

    for (const auto& header : options.extraHeaders)
        response.setHeader(header.first, header.second);
}

}  // namespace

StaticFileHandler::StaticFileHandler(std::string urlPrefix, std::string rootDir)
    : StaticFileHandler(std::move(urlPrefix), std::move(rootDir), Options{})
{
}

StaticFileHandler::StaticFileHandler(std::string urlPrefix, std::string rootDir, Options options)
    : urlPrefix_(std::move(urlPrefix)), rootDir_(std::move(rootDir)), options_(std::move(options))
{
    rootHandle_ = std::make_shared<RootHandle>(rootDir_);
    if (!urlPrefix_.empty() && urlPrefix_.back() == '/')
        urlPrefix_.pop_back();
}

bool StaticFileHandler::tryHandle(HttpContext& context) const
{
    const HttpRequest& request = context.request();
    if (request.method != HttpMethod::Get && request.method != HttpMethod::Head)
        return false;

    std::string_view path = request.path;
    if (path.substr(0, urlPrefix_.size()) != urlPrefix_)
        return false;

    if (path.size() > urlPrefix_.size() && path[urlPrefix_.size()] != '/')
        return false;

    std::string_view remainder = path.substr(std::min(path.size(), urlPrefix_.size()));

    std::string relative;
    if (!normalizeRelativePath(remainder, relative)) {
        context.status(400).text("Bad Request");
        return true;
    }
    if (!options_.allowDotfiles && hasDotComponent(relative))
        return false;

    if (!options_.extensionAllowlist.empty() && !relative.empty()) {
        const std::string extension = lowerExtension(relative);
        if (std::find(options_.extensionAllowlist.begin(), options_.extensionAllowlist.end(),
                      extension) == options_.extensionAllowlist.end())
            return false;
    }
#ifndef _WIN32
    struct Descriptor
    {
        int value;
        ~Descriptor()
        {
            if (value >= 0)
                ::close(value);
        }
    } file{::openat(rootHandle_->descriptor, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (file.value < 0) {
        context.status(500).text("Internal Server Error");
        return true;
    }
    const bool indexRequest = relative.empty();
    if (indexRequest)
        relative = "index.html";

    size_t start = 0;
    for (;;) {
        const size_t slash = relative.find('/', start);
        const std::string component =
            relative.substr(start, slash == std::string::npos ? slash : slash - start);
        const int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
                          (slash == std::string::npos ? 0 : O_DIRECTORY);
        int next = ::openat(file.value, component.c_str(), flags);
        if (next < 0 && indexRequest && errno == ENOENT) {
            relative = "index.htm";
            next = ::openat(file.value, relative.c_str(), flags);
        }
        if (next < 0) {
            if (errno == ENOENT)
                return false;

            context.status(403).text("Forbidden");
            return true;
        }
        ::close(file.value);
        file.value = next;
        if (slash == std::string::npos)
            break;

        start = slash + 1;
    }
    struct stat metadata
    {};
    if (::fstat(file.value, &metadata) != 0 || !S_ISREG(metadata.st_mode))
        return false;

    if (metadata.st_size < 0 || static_cast<uint64_t>(metadata.st_size) > options_.maxBytes) {
        context.status(413).text("Static file exceeds the configured service limit");
        return true;
    }
    const size_t fileSize = static_cast<size_t>(metadata.st_size);
    const std::string etag = makeEtag(fileSize, metadata.st_mtim.tv_sec, metadata.st_mtim.tv_nsec);
    context.response().setHeader("ETag", etag);
    applyFileHeaders(context, relative, options_);
    if (etagMatches(request.headers.get("If-None-Match"), etag)) {
        context.status(304);
        return true;
    }
    context.status(200);
    context.response().setHeader("Content-Type", std::string(mimeTypeForExtension(relative)));
    context.response().setHeader("Content-Length", std::to_string(fileSize));
    if (request.method == HttpMethod::Head)
        return true;

    auto& body = context.response().body;
    body.resize(fileSize);
    size_t received = 0;
    while (received < fileSize) {
        ssize_t count = ::read(file.value, body.data() + received, fileSize - received);
        if (count < 0 && errno == EINTR)
            continue;

        if (count <= 0) {
            context.status(500).text("Static file changed during read");
            context.response().setHeader("Content-Length", std::to_string(body.size()));
            return true;
        }
        received += static_cast<size_t>(count);
    }
    return true;
#else
    fs::path root = fs::path(rootDir_);

    // Root of the static prefix hit without a concrete resource (e.g. "/" or
    // "/static/") -- serve index.html/index.htm so an Angular/SPA-style app
    // mounted at rootDir_ loads from its root the way a browser expects.
    if (relative.empty()) {
        std::error_code indexEc;
        if (fs::is_regular_file(root / "index.htm", indexEc) &&
            !fs::is_regular_file(root / "index.html", indexEc)) {
            relative = "index.htm";
        } else {
            relative = "index.html";
        }
    }

    fs::path fullPath = root / relative;

    std::error_code ec;
    fs::path canonicalRoot = fs::weakly_canonical(root, ec);
    fs::path canonicalFile = fs::weakly_canonical(fullPath, ec);
    auto prefix = std::mismatch(canonicalRoot.begin(), canonicalRoot.end(), canonicalFile.begin(),
                                canonicalFile.end());
    if (ec || prefix.first != canonicalRoot.end()) {
        context.status(403).text("Forbidden");
        return true;
    }

    // No such file: decline, so the server's 404 (or onNotFound handler)
    // answers. A root-mounted static handler must not swallow every unmatched
    // path with a bare "Not Found".
    if (!fs::is_regular_file(canonicalFile, ec) || ec)
        return false;

    auto fileSize = fs::file_size(canonicalFile, ec);
    auto mtime = fs::last_write_time(canonicalFile, ec);
    std::string etag = makeEtag(static_cast<uint64_t>(fileSize),
                                static_cast<int64_t>(mtime.time_since_epoch().count()), 0);

    if (etagMatches(context.request().headers.get("If-None-Match"), etag)) {
        context.status(304);
        context.response().setHeader("ETag", etag);
        applyFileHeaders(context, relative, options_);
        return true;
    }

    context.status(200);
    context.response().setHeader("Content-Type",
                                 std::string(mimeTypeForExtension(canonicalFile.string())));
    context.response().setHeader("ETag", etag);
    applyFileHeaders(context, relative, options_);
    if (request.method == HttpMethod::Head) {
        context.response().setHeader("Content-Length", std::to_string(fileSize));
    } else {
        if (fileSize > options_.maxBytes) {
            context.status(413).text("Static file too large");
            return true;
        }
        std::ifstream file(canonicalFile, std::ios::binary);
        context.response().body.resize(static_cast<size_t>(fileSize));
        if (!file.read(context.response().body.data(), static_cast<std::streamsize>(fileSize)))
            context.status(500).text("Internal Server Error");
    }
    return true;
#endif
}

}  // namespace http
