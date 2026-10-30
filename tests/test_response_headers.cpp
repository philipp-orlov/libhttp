// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// EXT-07: response headers and cookies cannot carry line breaks or other
// separators, so client-supplied data placed in them cannot split a response.

#include "http/http_context.hpp"
#include "http/server.hpp"
#include "test_support.hpp"

#include <cstdlib>
#include <future>

using namespace testsupport;

namespace {
void expect(bool condition, int code, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::_Exit(code);
    }
}

template <class F>
bool rejects(F&& action)
{
    try {
        action();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}
}  // namespace

int main()
{
    http::HeaderList headers;
    expect(rejects([&] { headers.set("X-A", "ok\r\nSet-Cookie: pwn=1"); }), 1, "CRLF in value");
    expect(rejects([&] { headers.add("X-A", "line\nbreak"); }), 2, "LF in value");
    expect(rejects([&] { headers.set("X-A", std::string("nul\0byte", 8)); }), 3, "NUL in value");
    expect(rejects([&] { headers.set("X-A", "del\x7f"); }), 4, "DEL in value");
    expect(rejects([&] { headers.set("X A", "v"); }), 5, "space in name");
    expect(rejects([&] { headers.set("X-A:", "v"); }), 6, "colon in name");
    expect(rejects([&] { headers.set("", "v"); }), 7, "empty name");
    expect(headers.empty(), 8, "nothing was stored by a refused call");
    headers.set("X-Ok", "tab\tallowed; and text=\"quoted\"");
    expect(headers.get("x-ok") == "tab\tallowed; and text=\"quoted\"", 9, "ordinary value kept");

    http::HttpRequest request;
    http::HttpResponse response;
    http::HttpContext context(request, response);
    http::CookieOptions cookie;
    cookie.name = "session";
    cookie.value = "abc123";
    context.setCookie(cookie);
    expect(response.headers.get("Set-Cookie").find("session=abc123") == 0, 10, "valid cookie");
    cookie.value = "a; Path=/evil";
    expect(rejects([&] { context.setCookie(cookie); }), 11, "';' in cookie value");
    cookie.value = "x\r\nSet-Cookie: y=1";
    expect(rejects([&] { context.setCookie(cookie); }), 12, "CRLF in cookie value");
    cookie.value = "ok";
    cookie.name = "bad name";
    expect(rejects([&] { context.setCookie(cookie); }), 13, "space in cookie name");
    cookie.name = "n";
    cookie.domain = "example.com; Secure";
    expect(rejects([&] { context.setCookie(cookie); }), 14, "';' in cookie domain");
    cookie.domain.clear();
    cookie.sameSite = "Lax; Path=/x";
    expect(rejects([&] { context.setCookie(cookie); }), 15, "bad SameSite");
    expect(rejects([&] { context.text("x", "text/plain\r\nX-Injected: 1"); }), 16,
           "CRLF in content type");

    // Through a server: a handler that echoes client data into a header gets
    // an error response, never a split one.
    http::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = freePort();
    config.ioThreads = 1;
    config.workerThreads = 1;
    http::Server server(config);
    server.onGet("/redirect", [](http::HttpContext& context) {
        context.status(302);
        context.response().setHeader("Location", std::string(context.query("to")));
    });
    auto serving = std::async(std::launch::async, [&] { server.run(); });
    const std::string reply = roundTrip(
        config.port,
        "GET /redirect?to=%2Fok%0D%0ASet-Cookie:%20pwn%3D1 HTTP/1.1\r\nHost: x\r\n"
        "Connection: close\r\n\r\n");
    expect(reply.find("Set-Cookie") == std::string::npos, 17, "response was not split");
    expect(reply.find("503") != std::string::npos, 18, "refused header becomes an error response");
    const std::string ok = roundTrip(
        config.port, "GET /redirect?to=%2Fok HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    expect(ok.find("Location: /ok\r\n") != std::string::npos, 19, "valid Location still sent");
    server.stop();
    serving.get();
    std::puts("response headers: injection refused for headers, cookies and content types");
}
