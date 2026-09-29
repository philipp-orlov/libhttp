// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/internal/tls_stream.hpp"

#ifdef HTTP_WITH_SSL

#include <algorithm>
#include <stdexcept>
#include <openssl/err.h>

namespace http::internal {
namespace {
int noPassword(char*, int, int, void*)
{
    return 0;
}

int selectProtocol(SSL*, const unsigned char** output, unsigned char* outputLength,
                   const unsigned char* input, unsigned int inputLength, void*)
{
    static const unsigned char protocols[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    unsigned char* selected = nullptr;
    if (SSL_select_next_proto(&selected, outputLength, protocols, sizeof(protocols), input,
                              inputLength) != OPENSSL_NPN_NEGOTIATED)
        return SSL_TLSEXT_ERR_ALERT_FATAL;

    *output = selected;
    return SSL_TLSEXT_ERR_OK;
}
}  // namespace

TlsContext makeTlsContext(const std::string& certificate, const std::string& privateKey)
{
    TlsContext context(SSL_CTX_new(TLS_server_method()), SSL_CTX_free);
    if (!context)
        throw std::runtime_error("cannot create TLS context");

    SSL_CTX_set_default_passwd_cb(context.get(), noPassword);
    if (SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION) != 1 ||
        SSL_CTX_use_certificate_chain_file(context.get(), certificate.c_str()) != 1 ||
        SSL_CTX_use_PrivateKey_file(context.get(), privateKey.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(context.get()) != 1)
        throw std::runtime_error("invalid TLS certificate or private key configuration");

    SSL_CTX_set_options(context.get(), SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    SSL_CTX_set_alpn_select_cb(context.get(), selectProtocol, nullptr);
    return context;
}

TlsStream::TlsStream(std::shared_ptr<TcpConnection> connection, const TlsContext& context)
    : connection_(std::move(connection))
{
    if (!context)
        return;

    ssl_.reset(SSL_new(context.get()));
    std::unique_ptr<BIO, decltype(&BIO_free)> input(BIO_new(BIO_s_mem()), BIO_free);
    std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new(BIO_s_mem()), BIO_free);
    if (!ssl_ || !input || !output)
        throw std::runtime_error("cannot create TLS connection");

    BIO_set_mem_eof_return(input.get(), -1);
    BIO_set_mem_eof_return(output.get(), -1);
    SSL_set_bio(ssl_.get(), input.release(), output.release());
    SSL_set_accept_state(ssl_.get());
}

TlsStream::~TlsStream()
{
    connection_->close();
}

void TlsStream::asyncRead(unsigned char* buffer, size_t capacity, IoCallback callback)
{
    if (!ssl_) {
        connection_->asyncRead(buffer, capacity, std::move(callback));
        return;
    }
    if (closed_) {
        callback({0, false, 0});
        return;
    }
    if (!buffer) {
        buffer = plaintext_.data();
        capacity = plaintext_.size();
    }
    if (readCallback_ || !capacity)
        throw std::logic_error("invalid concurrent TLS read");

    readBuffer_ = buffer;
    readCapacity_ = capacity;
    readCallback_ = std::move(callback);
    pump();
}

void TlsStream::asyncWrite(const unsigned char* data, size_t length, IoCallback callback)
{
    if (!ssl_) {
        connection_->asyncWrite(data, length, std::move(callback));
        return;
    }
    if (closed_) {
        callback({0, false, 0});
        return;
    }
    if (writeCallback_)
        throw std::logic_error("invalid concurrent TLS write");

    writeBuffer_ = data;
    writeLength_ = length;
    writeOffset_ = 0;
    writeCallback_ = std::move(callback);
    pump();
}

size_t TlsStream::tryWrite(const unsigned char* head, size_t headLength, const unsigned char* body,
                           size_t bodyLength)
{
    if (ssl_ || closed_)
        return 0;

    return connection_->tryWrite(head, headLength, body, bodyLength);
}

bool TlsStream::flush()
{
    if (sending_)
        return false;

    const size_t pending = BIO_ctrl_pending(SSL_get_wbio(ssl_.get()));
    if (!pending)
        return true;

    outgoing_.resize(std::min(pending, size_t{65536}));
    const int count =
        BIO_read(SSL_get_wbio(ssl_.get()), outgoing_.data(), static_cast<int>(outgoing_.size()));
    if (count <= 0) {
        fail();
        return false;
    }
    sending_ = true;
    auto self = shared_from_this();
    connection_->asyncWrite(outgoing_.data(), static_cast<size_t>(count), [self](IoResult result) {
        self->sending_ = false;
        if (!result.ok) {
            self->fail();
            return;
        }
        if (self->closed_) {
            self->finishClose();
            return;
        }
        self->pump();
    });
    return !sending_ && !closed_;
}

void TlsStream::receive()
{
    if (receiving_ || closed_)
        return;

    receiving_ = true;
    auto self = shared_from_this();
    connection_->asyncRead(incoming_.data(), incoming_.size(), [self](IoResult result) {
        self->receiving_ = false;
        if (self->closed_)
            return;

        if (!result.ok || !result.bytes ||
            BIO_write(SSL_get_rbio(self->ssl_.get()), self->incoming_.data(),
                      static_cast<int>(result.bytes)) != static_cast<int>(result.bytes)) {
            self->fail();
            return;
        }
        self->pump();
    });
}

void TlsStream::pump()
{
    if (pumping_ || closed_)
        return;

    auto self = shared_from_this();
    pumping_ = true;
    try {
        size_t iterations = 0;
        while (!closed_) {
            if (++iterations > 64) {
                if (!connection_->loop().tryPost([self] { self->pump(); }))
                    fail();

                break;
            }
            if (!flush())
                break;

            ERR_clear_error();
            int result = 0;
            if (!SSL_is_init_finished(ssl_.get())) {
                result = SSL_do_handshake(ssl_.get());
                if (result == 1)
                    continue;

            } else if (writeCallback_) {
                if (writeOffset_ == writeLength_) {
                    auto callback = std::move(writeCallback_);
                    callback({writeLength_, true, 0});
                    continue;
                }
                result = SSL_write(
                    ssl_.get(), writeBuffer_ + writeOffset_,
                    static_cast<int>(std::min(writeLength_ - writeOffset_, size_t{16384})));
                if (result > 0) {
                    writeOffset_ += static_cast<size_t>(result);
                    continue;
                }
            } else if (readCallback_) {
                result = SSL_read(ssl_.get(), readBuffer_,
                                  static_cast<int>(std::min(readCapacity_, size_t{16384})));
                if (result > 0) {
                    auto callback = std::move(readCallback_);
                    callback({static_cast<size_t>(result), true, 0, readBuffer_});
                    continue;
                }
            } else {
                break;
            }
            const int error = SSL_get_error(ssl_.get(), result);
            if (error == SSL_ERROR_WANT_READ) {
                if (!flush())
                    break;

                receive();
                if (receiving_)
                    break;

            } else if (error == SSL_ERROR_WANT_WRITE) {
                if (!flush())
                    break;

            } else if (error == SSL_ERROR_ZERO_RETURN) {
                auto read = std::move(readCallback_);
                auto write = std::move(writeCallback_);
                close();
                if (read)
                    read({0, true, 0});

                if (write)
                    write({0, false, 0});

            } else {
                fail();
            }
        }
    } catch (...) {
        fail();
        pumping_ = false;
        throw;
    }
    pumping_ = false;
}

void TlsStream::fail()
{
    closed_ = true;
    finishClose();
    auto read = std::move(readCallback_);
    auto write = std::move(writeCallback_);
    if (read)
        read({0, false, 0});

    if (write)
        write({0, false, 0});
}

void TlsStream::finishClose()
{
    if (closeTimer_) {
        connection_->loop().cancelTimer(closeTimer_);
        closeTimer_ = 0;
    }
    connection_->close();
}

void TlsStream::close()
{
    if (closed_)
        return;

    closed_ = true;
    readCallback_ = {};
    writeCallback_ = {};
    if (!ssl_ || sending_ || !SSL_is_init_finished(ssl_.get())) {
        finishClose();
        return;
    }
    ERR_clear_error();
    SSL_shutdown(ssl_.get());
    if (!BIO_ctrl_pending(SSL_get_wbio(ssl_.get()))) {
        finishClose();
        return;
    }
    auto self = shared_from_this();
    try {
        closeTimer_ =
            connection_->loop().addTimer(std::chrono::seconds(1), [self] { self->finishClose(); });
        flush();
    } catch (...) {
        finishClose();
    }
}

}  // namespace http::internal

#else  // !HTTP_WITH_SSL

#include <stdexcept>
#include <utility>

// Built without OpenSSL: a plain passthrough onto the underlying
// TcpConnection (mirrors TlsStream's own !ssl_ branch above for a
// not-actually-TLS connection), so plain HTTP/WebSocket still work; only
// enabling TLS itself (a non-null TlsContext) fails, loudly, at that point.
namespace http::internal {

TlsContext makeTlsContext(const std::string&, const std::string&)
{
    throw std::runtime_error("TLS support not compiled in (built with HTTP_WITH_SSL=OFF)");
}

TlsStream::TlsStream(std::shared_ptr<TcpConnection> connection, const TlsContext&)
    : connection_(std::move(connection))
{}

TlsStream::~TlsStream()
{
    connection_->close();
}

void TlsStream::asyncRead(unsigned char* buffer, size_t capacity, IoCallback callback)
{
    connection_->asyncRead(buffer, capacity, std::move(callback));
}

void TlsStream::asyncWrite(const unsigned char* data, size_t length, IoCallback callback)
{
    connection_->asyncWrite(data, length, std::move(callback));
}

size_t TlsStream::tryWrite(const unsigned char* head, size_t headLength, const unsigned char* body,
                           size_t bodyLength)
{
    return connection_->tryWrite(head, headLength, body, bodyLength);
}

void TlsStream::close()
{
    connection_->close();
}

}  // namespace http::internal

#endif  // HTTP_WITH_SSL