// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/internal/tls_stream.hpp"
#include "http/server.hpp"
#include "http/http_context.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <openssl/pem.h>
#include <cerrno>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <thread>

namespace {
void checkAt(bool condition, int line)
{
    if (!condition)
        throw std::runtime_error("TLS transport assertion failed at line " + std::to_string(line));
}
#define check(condition) checkAt((condition), __LINE__)

uint16_t unusedPort()
{
    int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    check(descriptor >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(::bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    check(getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    ::close(descriptor);
    return ntohs(address.sin_port);
}

int connectTo(uint16_t port)
{
    for (size_t attempt = 0; attempt < 1000; ++attempt) {
        int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (::connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            timeval timeout{5, 0};
            setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            return descriptor;
        }
        ::close(descriptor);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("HTTPS listener did not start");
}

// OpenSSL over a blocking socket reports a read or write cut short by EINTR
// as "try again"; these retry the way a client has to. (A ring waiting in
// io_uring_enter on another thread of the process makes a timed socket read
// elsewhere in the process return EINTR once on some kernels -- Ubuntu's
// 6.8 does -- which is how the io_uring backend's build of this test found
// out.)
bool tlsRetry(SSL* ssl, int result)
{
    const int error = SSL_get_error(ssl, result);
    return error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE ||
           (error == SSL_ERROR_SYSCALL && errno == EINTR);
}

int tlsConnect(SSL* ssl)
{
    for (;;) {
        const int result = SSL_connect(ssl);
        if (result == 1 || !tlsRetry(ssl, result))
            return result;
    }
}

int tlsRead(SSL* ssl, void* buffer, int size)
{
    for (;;) {
        const int result = SSL_read(ssl, buffer, size);
        if (result > 0 || !tlsRetry(ssl, result))
            return result;
    }
}

int tlsWrite(SSL* ssl, const void* buffer, int size)
{
    for (;;) {
        const int result = SSL_write(ssl, buffer, size);
        if (result > 0 || !tlsRetry(ssl, result))
            return result;
    }
}

ssize_t recvRetrying(int socket, void* buffer, size_t size)
{
    ssize_t count;
    do {
        count = ::recv(socket, buffer, size, 0);
    } while (count < 0 && errno == EINTR);
    return count;
}

struct Client
{
    int descriptor;
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl;
    Client(uint16_t port, SSL_CTX* context)
        : descriptor(connectTo(port)), ssl(SSL_new(context), SSL_free)
    {
        check(ssl && SSL_set_fd(ssl.get(), descriptor) == 1 &&
              SSL_set1_host(ssl.get(), "localhost") == 1);
    }
    ~Client() { ::close(descriptor); }
    void send(const std::string& data)
    {
        check(tlsWrite(ssl.get(), data.data(), static_cast<int>(data.size())) ==
              static_cast<int>(data.size()));
    }
    std::string read(size_t size)
    {
        std::string data(size, '\0');
        size_t offset = 0;
        while (offset < size) {
            int count = tlsRead(ssl.get(), data.data() + offset, static_cast<int>(size - offset));
            check(count > 0);
            offset += static_cast<size_t>(count);
        }
        return data;
    }
    std::string headers()
    {
        std::string data;
        while (data.find("\r\n\r\n") == std::string::npos) {
            data += read(1);
            check(data.size() < 10000);
        }
        return data;
    }
};

void exerciseServer(http::Server& server, SSL_CTX* clientContext)
{
    server.onGet("/health", [](http::HttpContext& context) { context.text("healthy"); });
    server.onPost("/echo", [&server](http::HttpContext& context, std::function<void()> done) {
        server.pool().enqueue([&context, done] {
            context.loop().post([&context, done] {
                context.text(context.request().body.view());
                done();
            });
        });
    });
    server.onWebSocket("/ws", [](auto connection) {
        connection->send("ready");
        std::weak_ptr<http::WebSocketConnection> weak = connection;
        connection->onMessage([weak](std::string_view message, bool binary) {
            if (auto locked = weak.lock())
                locked->send(message, binary);
        });
    });
    auto serving = std::async(std::launch::async, [&] { server.run(); });
    try {
        const uint16_t port = server.config().tls->port;
        if (server.config().httpEnabled) {
            int plain = connectTo(server.config().port);
            const std::string request =
                "GET /health HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
            check(::send(plain, request.data(), request.size(), MSG_NOSIGNAL) ==
                  static_cast<ssize_t>(request.size()));
            std::string response;
            char bytes[4096];
            ssize_t count;
            while ((count = recvRetrying(plain, bytes, sizeof(bytes))) > 0)
                response.append(bytes, static_cast<size_t>(count));

            ::close(plain);
            check(response.find("HTTP/1.1 200") == 0 &&
                  response.find("healthy") != std::string::npos);
        }
        for (int version : {TLS1_2_VERSION, TLS1_3_VERSION}) {
            Client client(port, clientContext);
            SSL_set_min_proto_version(client.ssl.get(), version);
            SSL_set_max_proto_version(client.ssl.get(), version);
            const unsigned char protocols[] = {2,   'h', '2', 8,   'h', 't',
                                               't', 'p', '/', '1', '.', '1'};
            check(SSL_set_alpn_protos(client.ssl.get(), protocols, sizeof(protocols)) == 0);
            check(tlsConnect(client.ssl.get()) == 1);
            const unsigned char* selected = nullptr;
            unsigned int selectedLength = 0;
            SSL_get0_alpn_selected(client.ssl.get(), &selected, &selectedLength);
            check(std::string_view(reinterpret_cast<const char*>(selected), selectedLength) ==
                  "http/1.1");
            client.send("GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
            check(client.headers().find("HTTP/1.1 200") == 0 && client.read(7) == "healthy");
            std::string payload(300000, 'z');
            client.send("POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
                        std::to_string(payload.size()) + "\r\nConnection: close\r\n\r\n" + payload);
            check(client.headers().find("HTTP/1.1 200") == 0 &&
                  client.read(payload.size()) == payload);
            unsigned char byte;
            const int count = tlsRead(client.ssl.get(), &byte, 1);
            check(count == 0 && SSL_get_error(client.ssl.get(), count) == SSL_ERROR_ZERO_RETURN);
        }
        {
            Client client(port, clientContext);
            check(tlsConnect(client.ssl.get()) == 1);
            client.send(
                "GET /ws HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade\r\nUpgrade: "
                "websocket\r\n"
                "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
            check(client.headers().find("HTTP/1.1 101") == 0);
            check(client.read(7) == std::string("\x81\x05ready", 7));
            const char masked[] = {char(0x81),    char(0x82),   1, 2, 3, 4,
                                   char('h' ^ 1), char('i' ^ 2)};
            client.send(std::string(masked, sizeof(masked)));
            check(client.read(4) == std::string("\x81\x02hi", 4));
        }
        {
            Client wrongHost(port, clientContext);
            SSL_set1_host(wrongHost.ssl.get(), "wrong.example");
            check(SSL_connect(wrongHost.ssl.get()) != 1);
        }
        {
            std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> untrusted(
                SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
            SSL_CTX_set_verify(untrusted.get(), SSL_VERIFY_PEER, nullptr);
            Client client(port, untrusted.get());
            check(SSL_connect(client.ssl.get()) != 1);
        }
        {
            Client legacy(port, clientContext);
            SSL_set_security_level(legacy.ssl.get(), 0);
            SSL_set_max_proto_version(legacy.ssl.get(), TLS1_1_VERSION);
            check(SSL_connect(legacy.ssl.get()) != 1);
        }
        {
            Client unsupported(port, clientContext);
            const unsigned char protocols[] = {2, 'h', '2'};
            SSL_set_alpn_protos(unsupported.ssl.get(), protocols, sizeof(protocols));
            check(SSL_connect(unsupported.ssl.get()) != 1);
        }
        int invalid = connectTo(port);
        const std::string plaintext = "GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n";
        ::send(invalid, plaintext.data(), plaintext.size(), MSG_NOSIGNAL);
        char byte;
        check(recvRetrying(invalid, &byte, 1) <= 0);
        ::close(invalid);
        int stalled = connectTo(port);
        check(recvRetrying(stalled, &byte, 1) == 0);
        ::close(stalled);
        Client recovery(port, clientContext);
        check(tlsConnect(recovery.ssl.get()) == 1);
        recovery.send("GET /health HTTP/1.1\r\nHost: localhost\r\n\r\n");
        check(recovery.headers().find("HTTP/1.1 200") == 0 && recovery.read(7) == "healthy");
        const int shutdown = SSL_shutdown(recovery.ssl.get());
        check(shutdown == 1 || (shutdown == 0 && SSL_shutdown(recovery.ssl.get()) == 1));
        int unfinished = connectTo(port);
        server.stop();
        serving.get();
        ::close(unfinished);
        check(server.bufferStats().outstandingBlocks == 0);
    } catch (...) {
        server.stop();
        serving.get();
        throw;
    }
}
}  // namespace

int main()
{
    char certificatePath[] = "/tmp/libhttp-cert-XXXXXX";
    char keyPath[] = "/tmp/libhttp-key-XXXXXX";
    struct Cleanup
    {
        const char* certificate;
        const char* key;
        ~Cleanup()
        {
            unlink(certificate);
            unlink(key);
        }
    } cleanup{certificatePath, keyPath};
    FILE* certificateFile = fdopen(mkstemp(certificatePath), "w");
    FILE* keyFile = fdopen(mkstemp(keyPath), "w");
    check(certificateFile && keyFile);
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    check(generator && EVP_PKEY_keygen_init(generator.get()) == 1 &&
          EVP_PKEY_CTX_set_rsa_keygen_bits(generator.get(), 2048) == 1);
    EVP_PKEY* generated = nullptr;
    check(EVP_PKEY_keygen(generator.get(), &generated) == 1);
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(generated, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
    X509_set_version(certificate.get(), 2);
    ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1);
    X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60);
    X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600);
    X509_set_pubkey(certificate.get(), key.get());
    auto* name = X509_get_subject_name(certificate.get());
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    X509_set_issuer_name(certificate.get(), name);
    check(X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0);
    check(PEM_write_X509(certificateFile, certificate.get()) == 1);
    check(PEM_write_PrivateKey(keyFile, key.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1);
    fclose(certificateFile);
    fclose(keyFile);
    auto context = http::internal::makeTlsContext(certificatePath, keyPath);
    http::ServerConfig config;
    config.host = "127.0.0.1";
    config.port = unusedPort();
    config.ioThreads = 2;
    config.workerThreads = 1;
    config.requestTimeoutSeconds = 1;
    config.tls = http::TlsConfig{unusedPort(), certificatePath, keyPath};
    while (config.port == config.tls->port)
        config.tls->port = unusedPort();

    http::Server dual(config);
    {
        int occupied = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(::bind(occupied, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        check(::listen(occupied, 1) == 0);
        socklen_t size = sizeof(address);
        check(getsockname(occupied, reinterpret_cast<sockaddr*>(&address), &size) == 0);
        auto failedConfig = config;
        failedConfig.tls->port = ntohs(address.sin_port);
        http::Server failed(failedConfig);
        bool rejected = false;
        try {
            failed.run();
        } catch (const std::exception&) {
            rejected = true;
        }
        check(rejected);
        int probe = ::socket(AF_INET, SOCK_STREAM, 0);
        address.sin_port = htons(config.port);
        check(::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        ::close(probe);
        ::close(occupied);
    }
    config.httpEnabled = false;
    config.tls->port = unusedPort();
    http::Server secureOnly(config);
    auto rejects = [](http::ServerConfig invalid) {
        try {
            http::Server server(std::move(invalid));
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    auto invalid = config;
    invalid.tls.reset();
    check(rejects(invalid));
    invalid = config;
    invalid.httpEnabled = true;
    invalid.port = invalid.tls->port;
    check(rejects(invalid));
    invalid = config;
    invalid.tls->privateKeyFile = certificatePath;
    check(rejects(invalid));
    unlink(certificatePath);
    unlink(keyPath);
    check(rejects(config));

    int sockets[2];
    check(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    timeval timeout{5, 0};
    setsockopt(sockets[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sockets[1], SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    http::IoLoop loop;
    auto stream = std::make_shared<http::internal::TlsStream>(
        std::make_shared<http::TcpConnection>(loop, sockets[0]), context);
    std::string payload(100000, 'x');
    size_t received = 0;
    std::array<unsigned char, 4096> buffer{};
    std::function<void()> read;
    read = [&] {
        stream->asyncRead(buffer.data(), buffer.size(), [&](http::IoResult result) {
            check(result.ok && result.bytes);
            received += result.bytes;
            if (received < payload.size()) {
                read();
            } else {
                stream->asyncWrite(reinterpret_cast<const unsigned char*>(payload.data()),
                                   payload.size(), [&](http::IoResult written) {
                                       check(written.ok && written.bytes == payload.size());
                                       stream->close();
                                       loop.stop();
                                   });
            }
        });
    };
    loop.post(read);
    auto serving = std::async(std::launch::async, [&] { loop.run(); });
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> clientContext(
        SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
    check(X509_STORE_add_cert(SSL_CTX_get_cert_store(clientContext.get()), certificate.get()) == 1);
    SSL_CTX_set_verify(clientContext.get(), SSL_VERIFY_PEER, nullptr);
    std::unique_ptr<SSL, decltype(&SSL_free)> client(SSL_new(clientContext.get()), SSL_free);
    SSL_set_fd(client.get(), sockets[1]);
    SSL_set1_host(client.get(), "localhost");
    check(tlsConnect(client.get()) == 1);
    check(tlsWrite(client.get(), payload.data(), static_cast<int>(payload.size())) ==
          static_cast<int>(payload.size()));
    std::string response;
    std::array<unsigned char, 4096> clientBuffer{};
    for (;;) {
        const int count =
            tlsRead(client.get(), clientBuffer.data(), static_cast<int>(clientBuffer.size()));
        if (count <= 0) {
            check(SSL_get_error(client.get(), count) == SSL_ERROR_ZERO_RETURN);
            break;
        }
        response.append(reinterpret_cast<const char*>(clientBuffer.data()),
                        static_cast<size_t>(count));
    }
    check(response == payload);
    serving.get();
    close(sockets[1]);
    exerciseServer(dual, clientContext.get());
    exerciseServer(secureOnly, clientContext.get());
    std::puts(
        "TLS transport: verified certificate, handshake, multi-record upload/response and "
        "close_notify passed");
    std::puts(
        "HTTPS server: dual/TLS-only listeners, TLS 1.2/1.3, ALPN, keep-alive, async POST, WSS, "
        "rejection, timeout and shutdown passed");
}