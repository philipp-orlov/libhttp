// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/static_files.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {
struct Result
{
    bool handled = false;
    http::HttpResponse response;
};

// Runs one GET through a handler; not handled means it declined (-> 404).
Result run(const http::StaticFileHandler& handler, const char* path,
           const std::string& ifNoneMatch = {})
{
    // Headers are views into the request's own head copy.
    http::HttpRequest request;
    request.method = http::HttpMethod::Get;
    request.path = path;
    if (!ifNoneMatch.empty()) {
        request.head = ifNoneMatch;
        request.headers.add("If-None-Match", request.head);
    }
    Result result;
    http::HttpContext context(request, result.response);
    result.handled = handler.tryHandle(context);
    return result;
}
}  // namespace

int main()
{
    char name[] = "/tmp/http-static-XXXXXX";
    char* directory = ::mkdtemp(name);
    if (!directory)
        return 1;

    const std::filesystem::path root(directory);
    std::filesystem::create_directory(root / "public");
    std::filesystem::create_directory(root / "public-other");
    std::filesystem::create_directory(root / "public" / ".git");
    std::filesystem::create_directory(root / "public" / ".well-known");
    std::ofstream(root / "public" / "index.html") << "public";
    std::ofstream(root / "public" / ".env") << "SECRET=1";
    std::ofstream(root / "public" / ".git" / "config") << "[core]";
    std::ofstream(root / "public" / ".well-known" / "x.txt") << "wk";
    std::ofstream(root / "public" / "note.txt") << "note";
    std::ofstream(root / "public" / "big.bin") << std::string(2000, 'b');
    std::ofstream(root / "public" / "logo.svg") << "<svg/>";
    std::ofstream(root / "public-other" / "secret") << "secret";
    std::filesystem::create_symlink(root / "public-other" / "secret", root / "public" / "leak");
    const std::string publicDirectory = (root / "public").string();
    struct Cleanup
    {
        std::filesystem::path root;
        ~Cleanup() { std::filesystem::remove_all(root); }
    } cleanup{root};

    http::StaticFileHandler files("/", publicDirectory);
    auto check = [&](http::HttpMethod method, const char* path, int status, const char* body) {
        http::HttpRequest request;
        http::HttpResponse response;
        request.method = method;
        request.path = path;
        http::HttpContext context(request, response);
        return files.tryHandle(context) && response.status == status && response.body == body;
    };
    if (!(check(http::HttpMethod::Get, "/", 200, "public") &&
          check(http::HttpMethod::Head, "/", 200, "") &&
          check(http::HttpMethod::Get, "/leak", 403, "Forbidden")))
        return 2;

    // Dotfiles and dot directories are not served by default.
    for (const char* path : {"/.env", "/.git/config", "/.well-known/x.txt"})
        if (run(files, path).handled)
            return 3;

    http::StaticFileHandler::Options dotted;
    dotted.allowDotfiles = true;
    http::StaticFileHandler withDotfiles("/", publicDirectory, dotted);
    if (!run(withDotfiles, "/.well-known/x.txt").handled)
        return 4;

    // Extension allowlist.
    http::StaticFileHandler::Options only;
    only.extensionAllowlist = {"html", "txt"};
    http::StaticFileHandler allowlisted("/", publicDirectory, only);
    if (!run(allowlisted, "/note.txt").handled || run(allowlisted, "/big.bin").handled ||
        !run(allowlisted, "/").handled)
        return 5;

    // Size limit and configured headers.
    http::StaticFileHandler::Options limited;
    limited.maxBytes = 1000;
    limited.cacheControl = "no-store";
    limited.extraHeaders = {{"X-Frame-Options", "DENY"}};
    http::StaticFileHandler small("/", publicDirectory, limited);
    if (run(small, "/big.bin").response.status != 413)
        return 6;

    Result note = run(small, "/note.txt");
    if (note.response.status != 200 || note.response.headers.get("Cache-Control") != "no-store" ||
        note.response.headers.get("X-Frame-Options") != "DENY" ||
        note.response.headers.get("X-Content-Type-Options") != "nosniff")
        return 7;

    // The ETag is weak and carries neither the inode nor the raw mtime.
    const std::string etag(note.response.headers.get("ETag"));
    if (etag.rfind("W/\"", 0) != 0 || etag.size() != 3 + 16 + 1)
        return 8;

    // Conditional requests: exact, strong form, list and "*".
    for (const std::string& match : {etag, etag.substr(2), "\"other\", " + etag, std::string("*")})
        if (run(small, "/note.txt", match).response.status != 304)
            return 9;

    if (run(small, "/note.txt", "\"other\"").response.status != 200)
        return 10;

    // Same-origin active content is sandboxed.
    if (run(small, "/logo.svg").response.headers.get("Content-Security-Policy").find("sandbox") ==
        std::string_view::npos)
        return 11;

    std::puts(
        "static files: index, HEAD, symlink escape, dotfiles, allowlist, limits, ETag and headers "
        "passed");
}
