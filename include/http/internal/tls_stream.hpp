// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#ifdef HTTP_WITH_SSL
#include <openssl/ssl.h>
#endif

#include "http/io_loop.hpp"

namespace http::internal {

#ifdef HTTP_WITH_SSL
using TlsContext = std::shared_ptr<SSL_CTX>;
#else
// Built with HTTP_WITH_SSL=OFF: always null: TlsStream below is a plain
// passthrough and makeTlsContext() throws if anyone tries to use it.
using TlsContext = std::shared_ptr<void>;
#endif
TlsContext makeTlsContext(const std::string& certificate, const std::string& privateKey);

class TlsStream : public std::enable_shared_from_this<TlsStream>
{
public:
    TlsStream(std::shared_ptr<TcpConnection> connection, const TlsContext& context);
    ~TlsStream();
    void asyncRead(unsigned char* buffer, size_t capacity, IoCallback callback);
    void asyncWrite(const unsigned char* data, size_t length, IoCallback callback);
    // TcpConnection::tryWrite for a plain connection; 0 (nothing taken) over
    // TLS, where every byte goes through the record layer.
    size_t tryWrite(const unsigned char* head, size_t headLength, const unsigned char* body,
                    size_t bodyLength);
    void close();
    const std::string& peerAddress() const { return connection_->peerAddress(); }

private:
#ifdef HTTP_WITH_SSL
    void pump();
    bool flush();
    void receive();
    void fail();
    void finishClose();
#endif

    std::shared_ptr<TcpConnection> connection_;
#ifdef HTTP_WITH_SSL
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl_{nullptr, SSL_free};
    std::array<unsigned char, 16384> incoming_{};
    // Plaintext for a read issued without a buffer of its own (see
    // TcpConnection::asyncRead); a TLS connection has no shared buffer to
    // decrypt into, so it carries one.
    std::array<unsigned char, 16384> plaintext_{};
    std::vector<unsigned char> outgoing_;
    unsigned char* readBuffer_ = nullptr;
    size_t readCapacity_ = 0;
    IoCallback readCallback_;
    const unsigned char* writeBuffer_ = nullptr;
    size_t writeLength_ = 0;
    size_t writeOffset_ = 0;
    IoCallback writeCallback_;
    bool receiving_ = false;
    bool sending_ = false;
    bool pumping_ = false;
    bool closed_ = false;
    uint64_t closeTimer_ = 0;
#endif
};

}  // namespace http::internal