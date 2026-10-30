// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>
#include <string_view>
#include <memory>
#include <utility>
#include <vector>

#include "http/http_context.hpp"

namespace http {

std::string_view mimeTypeForExtension(std::string_view path);

// Serves files from a directory under a URL prefix, e.g. prefix "/static"
// + root "./public" maps GET /static/app.js -> ./public/app.js. Guards
// against path traversal by normalizing ".." segments in the (already
// URL-decoded) request path and rejecting any path that would escape the
// root. Linux pins the root descriptor and rejects symlinks during openat
// traversal; the Windows fallback uses canonical component containment.
class StaticFileHandler
{
public:
    struct Options
    {
        // A path with a component starting with '.' (/.git/config, /.env) is
        // not served unless this is set (e.g. for /.well-known/).
        bool allowDotfiles = false;
        // Lower-case extensions without the dot; when not empty only these
        // are served ("html", "js", "css", ...). Others are treated as absent.
        std::vector<std::string> extensionAllowlist;
        // A file larger than this is answered 413. Each response is held in
        // memory until it is sent, so this bounds concurrency x size.
        size_t maxBytes = 32 * 1024 * 1024;
        std::string cacheControl = "public, max-age=3600";
        // Added to every file response (Content-Security-Policy, ...).
        std::vector<std::pair<std::string, std::string>> extraHeaders;
    };

    StaticFileHandler(std::string urlPrefix, std::string rootDir);
    StaticFileHandler(std::string urlPrefix, std::string rootDir, Options options);

    // Returns true if this handler owns the request path (whether or not
    // the file was actually found -- a missing file under the prefix is a
    // handled 404, not a "not mine, try something else").
    bool tryHandle(HttpContext& context) const;

private:
    struct RootHandle;
    std::shared_ptr<RootHandle> rootHandle_;
    std::string urlPrefix_;
    std::string rootDir_;
    Options options_;
};

}  // namespace http
