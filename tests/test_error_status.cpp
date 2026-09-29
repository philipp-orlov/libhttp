// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// EXT-12: parse errors are answered with their own status and a generic body.

#include "http/http_context.hpp"
#include "http/server.hpp"
#include "test_support.hpp"

#include <cstdlib>
#include <future>
#include <vector>

using namespace testsupport;

namespace {
void expect(bool condition, int code, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::_Exit(code);
    }
}
}  // namespace

int main()
{
    http::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = freePort();
    config.ioThreads = 1;
    config.workerThreads = 1;
    config.maxHeaderBytes = 256;
    config.maxBodyBytes = 1000;
    http::Server server(config);
    server.onGet("/ping", [](http::HttpContext& context) { context.text("pong"); });
    server.onPost("/echo", [](http::HttpContext& context) { context.text("ok"); });
    auto serving = std::async(std::launch::async, [&] { server.run(); });

    struct Case
    {
        std::string request;
        std::string status;
        int code;
    };
    const std::vector<Case> cases = {
        {"GET /ping HTTP/2.0\r\nHost: x\r\n\r\n", "505 HTTP Version Not Supported", 1},
        {"GET /" + std::string(400, 'a') + " HTTP/1.1\r\nHost: x\r\n\r\n", "414 URI Too Long", 2},
        {"GET /ping HTTP/1.1\r\nHost: x\r\nX-Big: " + std::string(400, 'b') + "\r\n\r\n",
         "431 Request Header Fields Too Large", 3},
        {"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 5000\r\n\r\n", "413 Payload Too Large", 4},
        {"GET /ping HTTP/1.1\r\n\r\n", "400 Bad Request", 5},
        {"GET /ping HTTP/1.1\r\nHost x\r\n\r\n", "400 Bad Request", 6},
    };
    for (const Case& test : cases) {
        const std::string reply = roundTrip(config.port, test.request);
        expect(reply.rfind("HTTP/1.1 " + test.status, 0) == 0, test.code, test.status.c_str());
        // The body is the reason phrase, not the parser's internal wording.
        const size_t body = reply.find("\r\n\r\n");
        expect(body != std::string::npos &&
                   reply.substr(body + 4) == test.status.substr(test.status.find(' ') + 1),
               test.code + 10, "generic error body");
    }
    expect(roundTrip(config.port, "GET /ping HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
                   .find("pong") != std::string::npos,
           20, "normal request unaffected");
    server.stop();
    serving.get();
    std::puts("error status: 400/413/414/431/505 mapping and generic bodies passed");
}
