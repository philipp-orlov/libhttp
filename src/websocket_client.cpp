// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/websocket_client.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstring>
#include <mutex>
#include <random>

#include "http/base64.hpp"
#include "http/buffer.hpp"
#include "http/platform.hpp"
#include "http/websocket.hpp"

#if defined(HTTP_WITH_SSL) && !defined(HTTP_WINDOWS)
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#define HTTP_WS_CLIENT_TLS 1
#endif

#if !defined(HTTP_WINDOWS)
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#endif

namespace http {

namespace {

using Clock = std::chrono::steady_clock;
using Kind = WebSocketError::Kind;

bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

std::string_view trim(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

// Milliseconds from now until `when`, clamped to [0, int max] for poll().
int millisUntil(Clock::time_point when)
{
    if (when == Clock::time_point::max())
        return -1;
    // Rounded up: poll(2) with a truncated timeout returns just before the time is due, and a caller
    // that loops until then would spin.
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(when - Clock::now()).count();
    if (left <= 0)
        return 0;
    return left > 0x7fffffff ? 0x7fffffff : static_cast<int>(left);
}

}  // namespace

WebSocketUrl parseWebSocketUrl(std::string_view url)
{
    WebSocketUrl out;
    if (url.substr(0, 5) == "ws://") {
        url.remove_prefix(5);
    } else if (url.substr(0, 6) == "wss://") {
        out.secure = true;
        out.port = 443;
        url.remove_prefix(6);
    } else {
        throw WebSocketError(Kind::Usage, "not a ws:// or wss:// URL");
    }
    const size_t slash = url.find_first_of("/?");
    std::string_view authority = url.substr(0, slash);
    if (slash != std::string_view::npos) {
        out.target = std::string(url.substr(slash));
        if (out.target[0] == '?')
            out.target.insert(out.target.begin(), '/');
    }
    if (authority.find('@') != std::string_view::npos)
        throw WebSocketError(Kind::Usage, "credentials in the URL are not supported; use a header");
    std::string_view portText;
    if (!authority.empty() && authority.front() == '[') {
        const size_t close = authority.find(']');
        if (close == std::string_view::npos)
            throw WebSocketError(Kind::Usage, "bad IPv6 literal in the URL");
        out.host = std::string(authority.substr(1, close - 1));
        std::string_view rest = authority.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':')
                throw WebSocketError(Kind::Usage, "bad port in the URL");
            portText = rest.substr(1);
        }
    } else {
        const size_t colon = authority.rfind(':');
        out.host = std::string(authority.substr(0, colon));
        if (colon != std::string_view::npos)
            portText = authority.substr(colon + 1);
    }
    if (out.host.empty())
        throw WebSocketError(Kind::Usage, "no host in the URL");
    if (!portText.empty()) {
        unsigned value = 0;
        const auto parsed = std::from_chars(portText.data(), portText.data() + portText.size(), value);
        if (parsed.ec != std::errc() || parsed.ptr != portText.data() + portText.size() || value == 0 || value > 65535)
            throw WebSocketError(Kind::Usage, "bad port in the URL");
        out.port = static_cast<uint16_t>(value);
    }
    return out;
}

#if defined(HTTP_WINDOWS)

struct WebSocketClient::Impl
{};
WebSocketClient::WebSocketClient() = default;
WebSocketClient::~WebSocketClient() = default;
WebSocketClient::WebSocketClient(WebSocketClient&&) noexcept = default;
WebSocketClient& WebSocketClient::operator=(WebSocketClient&&) noexcept = default;
void WebSocketClient::connect(std::string_view, const WebSocketClientOptions&)
{
    throw WebSocketError(Kind::Usage, "WebSocketClient is not implemented on Windows yet");
}
bool WebSocketClient::isOpen() const noexcept { return false; }
const std::string& WebSocketClient::subprotocol() const noexcept
{
    static const std::string none;
    return none;
}
void WebSocketClient::sendBinary(const void*, size_t) {}
void WebSocketClient::sendText(std::string_view) {}
void WebSocketClient::ping(std::string_view) {}
std::optional<WebSocketClient::Message> WebSocketClient::receive(Clock::time_point, std::chrono::milliseconds)
{
    return std::nullopt;
}
bool WebSocketClient::peerClosed() { return true; }
void WebSocketClient::close(uint16_t, std::string_view) noexcept {}

#else

#ifdef HTTP_WS_CLIENT_TLS
// Holds SIGPIPE back on this thread for a scope; one raised by the writes inside is swallowed.
struct NoSigpipe
{
    sigset_t pipe, before;
    bool wasPending = false;
    NoSigpipe()
    {
        sigemptyset(&pipe);
        sigaddset(&pipe, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &pipe, &before);
        sigset_t pending;
        sigpending(&pending);
        wasPending = sigismember(&pending, SIGPIPE) == 1;
    }
    ~NoSigpipe()
    {
        if (!wasPending && sigismember(&before, SIGPIPE) != 1) {
            const timespec none{0, 0};
            sigtimedwait(&pipe, nullptr, &none);
        }
        pthread_sigmask(SIG_SETMASK, &before, nullptr);
    }
};
#endif

struct WebSocketClient::Impl
{
    int fd = -1;
    // open: the connection is usable. Cleared by abortSocket() from any thread; the descriptor and the TLS state
    // are only released by releaseSocket() (destructor, connect), when no other thread is inside the client.
    std::atomic<bool> open{false};
    bool secure = false;
    std::string subprotocol;
    size_t maxMessage = 0;
    std::mutex sendMutex;
    std::string sendScratch;       // masked outgoing frame, kept
    std::string in;                // bytes read and not yet consumed (frames being assembled)
    size_t inPos = 0;
    Buffer message;                // the assembled message, 8-aligned (pooled)
    size_t messageSize = 0;
    bool messageBinary = true;
    bool assembling = false;
    uint32_t maskState = 0x9e3779b9u;
    uint16_t closeCode = 0;
    std::string closeReason;

    ~Impl() { releaseSocket(); }

#ifdef HTTP_WS_CLIENT_TLS
    // OpenSSL objects are not safe for a reader and a writer at once, so every SSL_* call on `ssl`
    // runs under tlsMutex and the socket is non-blocking: a call that would wait releases the
    // mutex and polls instead.
    SSL_CTX* sslContext = nullptr;
    SSL* ssl = nullptr;
    std::mutex tlsMutex;

    static std::string tlsErrors(const char* what)
    {
        std::string text = what;
        char line[256];
        while (const unsigned long code = ERR_get_error()) {
            ERR_error_string_n(code, line, sizeof(line));
            text += ": ";
            text += line;
            break;
        }
        ERR_clear_error();
        return text;
    }

    // Waits for the socket in the direction OpenSSL asked for.
    void tlsWait(int sslError, Clock::time_point deadline)
    {
        pollfd p{fd, static_cast<short>(sslError == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN), 0};
        const int ms = deadline == Clock::time_point::max() ? 1000 : millisUntil(deadline);
        const int ready = ::poll(&p, 1, ms);
        if (ready < 0 && errno != EINTR)
            throw WebSocketError(Kind::Io, std::string("poll failed: ") + std::strerror(errno));
    }

    void tlsHandshake(const WebSocketUrl& url, const WebSocketClientOptions& options, Clock::time_point deadline)
    {
        sslContext = SSL_CTX_new(TLS_client_method());
        if (!sslContext)
            throw WebSocketError(Kind::Tls, tlsErrors("SSL_CTX_new"));
        SSL_CTX_set_min_proto_version(sslContext, TLS1_2_VERSION);
        SSL_CTX_set_mode(sslContext, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
        SSL_CTX_set_options(sslContext, SSL_OP_IGNORE_UNEXPECTED_EOF);
        if (options.insecureSkipVerify) {
            SSL_CTX_set_verify(sslContext, SSL_VERIFY_NONE, nullptr);
        } else {
            SSL_CTX_set_verify(sslContext, SSL_VERIFY_PEER, nullptr);
            const int loaded = options.caFile.empty() ? SSL_CTX_set_default_verify_paths(sslContext)
                                                      : SSL_CTX_load_verify_locations(sslContext, options.caFile.c_str(), nullptr);
            if (loaded != 1)
                throw WebSocketError(Kind::Tls, tlsErrors("cannot load the trusted certificates"));
        }
        ssl = SSL_new(sslContext);
        if (!ssl || SSL_set_fd(ssl, fd) != 1)
            throw WebSocketError(Kind::Tls, tlsErrors("SSL_new"));
        // Host name checks (an IP literal is matched against the certificate's IP entries).
        in6_addr probe;
        const bool literal = ::inet_pton(AF_INET, url.host.c_str(), &probe) == 1 || ::inet_pton(AF_INET6, url.host.c_str(), &probe) == 1;
        if (!options.insecureSkipVerify) {
            const int set = literal ? X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), url.host.c_str())
                                    : SSL_set1_host(ssl, url.host.c_str());
            if (set != 1)
                throw WebSocketError(Kind::Tls, tlsErrors("cannot set the expected host name"));
        }
        if (!literal)
            SSL_set_tlsext_host_name(ssl, url.host.c_str());
        SSL_set_connect_state(ssl);
        for (;;) {
            int result, error;
            {
                std::lock_guard<std::mutex> lock(tlsMutex);
                ERR_clear_error();
                result = SSL_connect(ssl);
                error = result == 1 ? SSL_ERROR_NONE : SSL_get_error(ssl, result);
            }
            if (result == 1)
                return;
            if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
                std::string why = tlsErrors("TLS handshake failed");
                if (const long verify = SSL_get_verify_result(ssl); verify != X509_V_OK)
                    why += std::string(" (certificate: ") + X509_verify_cert_error_string(verify) + ")";
                throw WebSocketError(Kind::Tls, why);
            }
            if (Clock::now() >= deadline)
                throw WebSocketError(Kind::Connect, "the TLS handshake timed out");
            tlsWait(error, deadline);
        }
    }

    void tlsSendAll(const char* data, size_t size)
    {
        while (size) {
            int sent, error = SSL_ERROR_NONE;
            {
                std::lock_guard<std::mutex> lock(tlsMutex);
                if (!ssl)
                    throw WebSocketError(Kind::Io, "send failed: the connection is closed");
                ERR_clear_error();
                NoSigpipe guard;  // SSL_write uses write(2), which raises SIGPIPE on a dead peer
                sent = SSL_write(ssl, data, static_cast<int>(std::min<size_t>(size, 1u << 30)));
                if (sent <= 0)
                    error = SSL_get_error(ssl, sent);
            }
            if (sent > 0) {
                data += sent;
                size -= static_cast<size_t>(sent);
                continue;
            }
            if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
                tlsWait(error, Clock::time_point::max());
                continue;
            }
            abortSocket();
            throw WebSocketError(Kind::Io, tlsErrors("TLS send failed"));
        }
    }

    // One SSL_read into `chunk`; 0 when nothing is ready, -1 at the end of the stream.
    ssize_t tlsRecv(char* chunk, size_t size)
    {
        std::lock_guard<std::mutex> lock(tlsMutex);
        if (!ssl)
            return -1;
        ERR_clear_error();
        const int got = SSL_read(ssl, chunk, static_cast<int>(size));
        if (got > 0)
            return got;
        const int error = SSL_get_error(ssl, got);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
            return 0;
        if (error == SSL_ERROR_ZERO_RETURN || (error == SSL_ERROR_SYSCALL && errno == 0))
            return -1;
        throw WebSocketError(Kind::Io, tlsErrors("TLS receive failed"));
    }

    bool tlsPending()
    {
        std::lock_guard<std::mutex> lock(tlsMutex);
        return ssl && SSL_pending(ssl) > 0;
    }
#endif

    // Ends the connection without freeing anything, so it is safe while another thread is in receive() (which
    // wakes with an error) or in a send (which fails): shutdown(2) on the descriptor, which stays open until
    // releaseSocket(). Thread-safe.
    void abortSocket() noexcept
    {
        open = false;
        const int descriptor = fd;
        if (descriptor >= 0)
            ::shutdown(descriptor, SHUT_RDWR);
    }

    void releaseSocket() noexcept
    {
        open = false;
#ifdef HTTP_WS_CLIENT_TLS
        {
            // One critical section: another thread may be inside SSL_read or SSL_write.
            std::lock_guard<std::mutex> lock(tlsMutex);
            if (ssl) {
                if (fd >= 0) {
                    NoSigpipe guard;
                    SSL_shutdown(ssl);
                }  // close_notify, best effort; the socket is non-blocking
                SSL_free(ssl);
                ssl = nullptr;
            }
            if (sslContext) {
                SSL_CTX_free(sslContext);
                sslContext = nullptr;
            }
        }
#endif
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    uint32_t nextMask() noexcept
    {
        // xorshift32: masks only have to be unpredictable to an intermediary, and each
        // connection is seeded from the system's entropy.
        maskState ^= maskState << 13;
        maskState ^= maskState >> 17;
        maskState ^= maskState << 5;
        return maskState;
    }

    void sendAll(const char* data, size_t size)
    {
#ifdef HTTP_WS_CLIENT_TLS
        if (secure) {
            tlsSendAll(data, size);
            return;
        }
#endif
        while (size) {
            const ssize_t sent = ::send(fd, data, size, MSG_NOSIGNAL);
            if (sent < 0) {
                if (errno == EINTR)
                    continue;
                abortSocket();
                throw WebSocketError(Kind::Io, std::string("send failed: ") + std::strerror(errno));
            }
            data += sent;
            size -= static_cast<size_t>(sent);
        }
    }

    // Masked client frame; `fin` and opcode as given. Thread-safe.
    void sendFrame(WsOpcode opcode, const char* payload, size_t size)
    {
        std::lock_guard<std::mutex> lock(sendMutex);
        if (!open)
            throw WebSocketError(Kind::Usage, "the WebSocket is closed");
        char header[14];
        size_t headerSize = encodeFrameHeader(opcode, size, true, header);
        header[1] = static_cast<char>(header[1] | 0x80);
        const uint32_t mask = nextMask();
        uint8_t key[4];
        std::memcpy(key, &mask, 4);
        std::memcpy(header + headerSize, key, 4);
        headerSize += 4;
        sendScratch.resize(headerSize + size);
        std::memcpy(sendScratch.data(), header, headerSize);
        if (size)
            std::memcpy(sendScratch.data() + headerSize, payload, size);
        applyMask(sendScratch.data() + headerSize, size, key);
        sendAll(sendScratch.data(), sendScratch.size());
        if (sendScratch.capacity() > (1u << 20) && size < (1u << 16)) {
            std::string().swap(sendScratch);  // one huge message must not pin a megabyte for ever
        }
    }

    // Reads what the socket has (blocking up to `deadline`); false on timeout.
    bool fill(Clock::time_point deadline)
    {
#ifdef HTTP_WS_CLIENT_TLS
        if (secure && tlsPending())
            return readSome();
#endif
        pollfd p{fd, POLLIN, 0};
        for (;;) {
            const int ready = ::poll(&p, 1, millisUntil(deadline));
            if (ready < 0) {
                if (errno == EINTR)
                    continue;
                abortSocket();
                throw WebSocketError(Kind::Io, std::string("poll failed: ") + std::strerror(errno));
            }
            if (ready == 0)
                return false;
            break;
        }
        return readSome();
    }

    bool readSome()
    {
        if (inPos > 0 && inPos == in.size()) {
            in.clear();
            inPos = 0;
        } else if (inPos > (1u << 16)) {
            in.erase(0, inPos);
            inPos = 0;
        }
        char chunk[16384];
#ifdef HTTP_WS_CLIENT_TLS
        if (secure) {
            ssize_t got;
            try {
                got = tlsRecv(chunk, sizeof(chunk));
            } catch (...) {
                abortSocket();
                throw;
            }
            if (got < 0) {
                abortSocket();
                throw WebSocketError(Kind::Closed, "the connection was closed", 0, closeCode ? closeCode : 1006, closeReason);
            }
            if (got > 0)
                in.append(chunk, static_cast<size_t>(got));
            return true;
        }
#endif
        const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                return true;
            abortSocket();
            throw WebSocketError(Kind::Io, std::string("recv failed: ") + std::strerror(errno));
        }
        if (got == 0) {
            abortSocket();
            throw WebSocketError(Kind::Closed, "the connection was closed", 0, closeCode ? closeCode : 1006, closeReason);
        }
        in.append(chunk, static_cast<size_t>(got));
        return true;
    }

    void appendMessage(const char* data, size_t size)
    {
        if (size > maxMessage - messageSize) {
            sendCloseFrame(1009, "message too big");
            abortSocket();
            throw WebSocketError(Kind::TooBig, "a message exceeds the configured maximum");
        }
        const size_t need = messageSize + size;
        if (message.capacity() < need) {
            Buffer bigger(message.capacity() ? Buffer(*pool, std::max(need, message.capacity() * 2)) : Buffer(*pool, std::max<size_t>(need, 4096)));
            if (messageSize)
                std::memcpy(bigger.data(), message.data(), messageSize);
            message = std::move(bigger);
        }
        if (size)
            std::memcpy(message.data() + messageSize, data, size);
        messageSize = need;
    }

    void sendCloseFrame(uint16_t code, std::string_view reason) noexcept
    {
        try {
            char payload[125];
            payload[0] = static_cast<char>(code >> 8);
            payload[1] = static_cast<char>(code & 0xff);
            const size_t n = std::min<size_t>(reason.size(), 123);
            if (n)
                std::memcpy(payload + 2, reason.data(), n);
            sendFrame(WsOpcode::Close, payload, 2 + n);
        } catch (...) {
        }
    }

    BufferPool* pool = nullptr;

    // Whether a Close frame is among the complete frames in `in`.
    bool closeFrameBuffered() const
    {
        size_t at = inPos;
        while (at < in.size()) {
            WsFrameHeader hdr;
            try {
                if (!tryParseFrameHeader(std::string_view(in).substr(at), hdr))
                    return false;
            } catch (...) {
                return true;
            }
            if (hdr.opcode == WsOpcode::Close)
                return true;
            at += hdr.headerSize + static_cast<size_t>(hdr.payloadLen);
        }
        return false;
    }
};

WebSocketClient::WebSocketClient() : impl_(std::make_unique<Impl>()) {}
WebSocketClient::~WebSocketClient()
{
    if (impl_)
        close();
}
WebSocketClient::WebSocketClient(WebSocketClient&&) noexcept = default;
WebSocketClient& WebSocketClient::operator=(WebSocketClient&& other) noexcept
{
    if (this != &other) {
        if (impl_)
            close();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

bool WebSocketClient::isOpen() const noexcept { return impl_ && impl_->open; }

const std::string& WebSocketClient::subprotocol() const noexcept
{
    static const std::string none;
    return impl_ ? impl_->subprotocol : none;
}

void WebSocketClient::connect(std::string_view urlText, const WebSocketClientOptions& options)
{
    Impl& m = *impl_;
    if (m.open)
        throw WebSocketError(Kind::Usage, "already connected");
    m.releaseSocket();  // a previous connection's descriptor
    const WebSocketUrl url = parseWebSocketUrl(urlText);
#ifndef HTTP_WS_CLIENT_TLS
    if (url.secure)
        throw WebSocketError(Kind::Tls, "wss:// needs a libhttp built with HTTP_WITH_SSL");
#endif
    m.secure = url.secure;
    m.pool = options.pool ? options.pool : &BufferPool::shared();
    m.maxMessage = options.maxMessageBytes;
    m.subprotocol.clear();
    m.in.clear();
    m.inPos = 0;
    m.messageSize = 0;
    m.assembling = false;
    m.closeCode = 0;
    m.closeReason.clear();
    {
        std::random_device source;
        m.maskState = (static_cast<uint32_t>(source()) | 1u);
    }

    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    const std::string port = std::to_string(url.port);
    if (const int rc = ::getaddrinfo(url.host.c_str(), port.c_str(), &hints, &found); rc != 0 || !found)
        throw WebSocketError(Kind::Connect, "cannot resolve " + url.host + ": " + ::gai_strerror(rc));
    const Clock::time_point deadline = Clock::now() + options.connectTimeout;
    std::string lastError = "no address";
    int fd = -1;
    for (addrinfo* a = found; a && fd < 0; a = a->ai_next) {
        const int s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s < 0) {
            lastError = std::strerror(errno);
            continue;
        }
        const int flags = ::fcntl(s, F_GETFL, 0);
        ::fcntl(s, F_SETFL, flags | O_NONBLOCK);
        int rc = ::connect(s, a->ai_addr, a->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            pollfd p{s, POLLOUT, 0};
            rc = ::poll(&p, 1, millisUntil(deadline));
            if (rc > 0) {
                int error = 0;
                socklen_t size = sizeof(error);
                ::getsockopt(s, SOL_SOCKET, SO_ERROR, &error, &size);
                if (error == 0) {
                    rc = 0;
                } else {
                    lastError = std::strerror(error);
                    rc = -1;
                }
            } else {
                lastError = rc == 0 ? "connect timed out" : std::strerror(errno);
                rc = -1;
            }
        } else if (rc < 0) {
            lastError = std::strerror(errno);
        }
        if (rc == 0) {
            if (!url.secure)
                ::fcntl(s, F_SETFL, flags);  // TLS keeps the socket non-blocking
            fd = s;
        } else {
            ::close(s);
        }
    }
    ::freeaddrinfo(found);
    if (fd < 0)
        throw WebSocketError(Kind::Connect, "cannot connect to " + url.host + ":" + port + ": " + lastError);
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    m.fd = fd;

    // Handshake.
    uint8_t nonce[16];
    {
        std::random_device source;
        for (uint8_t& b : nonce)
            b = static_cast<uint8_t>(source());
    }
    const std::string key = base64Encode(nonce, sizeof(nonce));
    std::string request = "GET " + url.target + " HTTP/1.1\r\nHost: " + url.host;
    if (url.port != (url.secure ? 443 : 80))
        request += ":" + port;
    request += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " + key + "\r\n";
    if (!options.subprotocols.empty()) {
        request += "Sec-WebSocket-Protocol: ";
        for (size_t i = 0; i < options.subprotocols.size(); ++i)
            request += (i ? ", " : "") + options.subprotocols[i];
        request += "\r\n";
    }
    for (const auto& [name, value] : options.headers)
        request += name + ": " + value + "\r\n";
    request += "\r\n";
    try {
#ifdef HTTP_WS_CLIENT_TLS
        if (url.secure)
            m.tlsHandshake(url, options, deadline);
#endif
        m.open = true;
        m.sendAll(request.data(), request.size());
        size_t end;
        while ((end = m.in.find("\r\n\r\n")) == std::string::npos) {
            if (m.in.size() > 64 * 1024)
                throw WebSocketError(Kind::Handshake, "the handshake response is too large");
            if (!m.fill(deadline))
                throw WebSocketError(Kind::Connect, "the server did not answer the handshake in time");
        }
        const std::string_view head(m.in.data(), end);
        const size_t eol = head.find("\r\n");
        const std::string_view status = head.substr(0, eol);
        int code = 0;
        if (status.size() >= 12 && status.substr(0, 5) == "HTTP/")
            std::from_chars(status.data() + 9, status.data() + 12, code);
        if (code != 101)
            throw WebSocketError(Kind::Handshake, "the server refused the upgrade: " + std::string(status), code);
        std::string_view accept, protocol;
        bool upgrade = false;
        std::string_view rest = eol == std::string_view::npos ? std::string_view() : head.substr(eol + 2);
        while (!rest.empty()) {
            const size_t next = rest.find("\r\n");
            const std::string_view line = rest.substr(0, next);
            rest = next == std::string_view::npos ? std::string_view() : rest.substr(next + 2);
            const size_t colon = line.find(':');
            if (colon == std::string_view::npos)
                continue;
            const std::string_view name = line.substr(0, colon), value = trim(line.substr(colon + 1));
            if (iequals(name, "Sec-WebSocket-Accept"))
                accept = value;
            else if (iequals(name, "Sec-WebSocket-Protocol"))
                protocol = value;
            else if (iequals(name, "Upgrade"))
                upgrade = iequals(value, "websocket");
        }
        if (!upgrade || accept != computeWebSocketAccept(key))
            throw WebSocketError(Kind::Handshake, "the server's upgrade response is not valid", code);
        if (!protocol.empty()) {
            if (std::find(options.subprotocols.begin(), options.subprotocols.end(), protocol) == options.subprotocols.end())
                throw WebSocketError(Kind::Handshake, "the server chose a sub-protocol that was not offered", code);
            m.subprotocol = std::string(protocol);
        }
        m.inPos = end + 4;  // frames sent on the heels of the 101 are already in `in`
    } catch (...) {
        m.releaseSocket();
        throw;
    }
}

void WebSocketClient::sendBinary(const void* data, size_t size)
{
    impl_->sendFrame(WsOpcode::Binary, static_cast<const char*>(data), size);
}

void WebSocketClient::sendText(std::string_view text)
{
    impl_->sendFrame(WsOpcode::Text, text.data(), text.size());
}

void WebSocketClient::ping(std::string_view payload)
{
    impl_->sendFrame(WsOpcode::Ping, payload.data(), std::min<size_t>(payload.size(), 125));
}

std::optional<WebSocketClient::Message> WebSocketClient::receive(Clock::time_point deadline,
                                                                std::chrono::milliseconds pingEvery)
{
    Impl& m = *impl_;
    if (!m.open)
        throw WebSocketError(Kind::Usage, "the WebSocket is closed");
    Clock::time_point lastHeard = Clock::now();
    for (;;) {
        // A complete frame in `in`?
        WsFrameHeader hdr;
        bool have = false;
        try {
            have = tryParseFrameHeader(std::string_view(m.in).substr(m.inPos), hdr);
        } catch (const std::exception&) {
            m.sendCloseFrame(1002, "bad frame");
            m.abortSocket();
            throw WebSocketError(Kind::Protocol, "the server sent a malformed frame");
        }
        if (have) {
            if (hdr.masked) {
                m.sendCloseFrame(1002, "masked frame from server");
                m.abortSocket();
                throw WebSocketError(Kind::Protocol, "the server sent a masked frame");
            }
            if (hdr.payloadLen > m.maxMessage) {
                m.sendCloseFrame(1009, "message too big");
                m.abortSocket();
                throw WebSocketError(Kind::TooBig, "a frame exceeds the configured maximum");
            }
            const size_t total = hdr.headerSize + static_cast<size_t>(hdr.payloadLen);
            if (m.in.size() - m.inPos >= total) {
                const char* payload = m.in.data() + m.inPos + hdr.headerSize;
                const size_t size = static_cast<size_t>(hdr.payloadLen);
                m.inPos += total;
                lastHeard = Clock::now();
                switch (hdr.opcode) {
                    case WsOpcode::Text:
                    case WsOpcode::Binary:
                        if (m.assembling) {
                            m.sendCloseFrame(1002, "unexpected data frame");
                            m.abortSocket();
                            throw WebSocketError(Kind::Protocol, "a data frame arrived inside a fragmented message");
                        }
                        m.messageSize = 0;
                        m.messageBinary = hdr.opcode == WsOpcode::Binary;
                        m.appendMessage(payload, size);
                        m.assembling = !hdr.fin;
                        break;
                    case WsOpcode::Continuation:
                        if (!m.assembling) {
                            m.sendCloseFrame(1002, "unexpected continuation");
                            m.abortSocket();
                            throw WebSocketError(Kind::Protocol, "a continuation frame without a message");
                        }
                        m.appendMessage(payload, size);
                        m.assembling = !hdr.fin;
                        break;
                    case WsOpcode::Ping: m.sendFrame(WsOpcode::Pong, payload, size); continue;
                    case WsOpcode::Pong: continue;
                    case WsOpcode::Close: {
                        m.closeCode = size >= 2 ? static_cast<uint16_t>((static_cast<uint8_t>(payload[0]) << 8) | static_cast<uint8_t>(payload[1])) : 1005;
                        m.closeReason = size > 2 ? std::string(payload + 2, size - 2) : std::string();
                        m.sendCloseFrame(m.closeCode == 1005 ? 1000 : m.closeCode, "");
                        m.abortSocket();
                        throw WebSocketError(Kind::Closed, "the server closed the connection", 0, m.closeCode, m.closeReason);
                    }
                    default:
                        m.sendCloseFrame(1002, "unknown opcode");
                        m.abortSocket();
                        throw WebSocketError(Kind::Protocol, "the server sent an unknown opcode");
                }
                if (m.assembling)
                    continue;
                // Keep the staging string from pinning memory after a big message.
                if (m.in.capacity() > (1u << 20) && m.inPos == m.in.size()) {
                    std::string().swap(m.in);
                    m.inPos = 0;
                }
                return Message{std::string_view(reinterpret_cast<const char*>(m.message.data()), m.messageSize), m.messageBinary};
            }
        }
        // Need more bytes: wait for the socket, a ping due, or the deadline.
        Clock::time_point wake = deadline;
        if (pingEvery.count() > 0)
            wake = std::min(wake, lastHeard + pingEvery);
        if (!m.fill(wake)) {
            const Clock::time_point now = Clock::now();
            if (now >= deadline)
                return std::nullopt;
            if (pingEvery.count() > 0 && now >= lastHeard + pingEvery) {
                m.sendFrame(WsOpcode::Ping, nullptr, 0);
                lastHeard = now;
            }
        }
    }
}

bool WebSocketClient::peerClosed()
{
    Impl& m = *impl_;
    if (!m.open)
        return true;
    try {
        pollfd p{m.fd, POLLIN, 0};
        while (
#ifdef HTTP_WS_CLIENT_TLS
            (m.secure && m.tlsPending()) ||
#endif
            (::poll(&p, 1, 0) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR)))) {
            m.readSome();  // throws Closed on end of stream
            p.revents = 0;
        }
        return m.closeFrameBuffered();
    } catch (const WebSocketError&) {
        return true;
    }
}

void WebSocketClient::close(uint16_t code, std::string_view reason) noexcept
{
    Impl& m = *impl_;
    if (m.open)
        m.sendCloseFrame(code, reason);
    m.abortSocket();  // safe against a thread blocked in receive(): it wakes with Closed
}

#endif

}  // namespace http
