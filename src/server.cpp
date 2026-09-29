// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/server.hpp"

#include <cstring>
#include <deque>
#include <exception>
#include <optional>
#include <unordered_map>

#include "http/http_context.hpp"
#include "http/base64.hpp"
#include "http/utf8.hpp"
#include "http/http_parser.hpp"
#include "http/logger.hpp"
#include "http/websocket.hpp"
#include "http/internal/tls_stream.hpp"

namespace http {

struct internal::ServerTls
{
    TlsContext context;
};

namespace {
// Most bytes taken by one read straight into a request body or a partial
// WebSocket frame (reads that consume input on the spot use the loop's own
// receive buffer and have no such limit -- see TcpConnection::asyncRead).
constexpr size_t kReadChunkSize = 256 * 1024;

// A response body up to this size is corked along with its head; a larger
// one is written from the handler's string rather than copied.
constexpr size_t kMaxCoalescedBodyBytes = 64 * 1024;

// Corked output is handed to the socket once it reaches this much even if
// there is still input to answer; and if the socket has not taken the
// previous batch by then, parsing pauses until it has (see processHttp).
constexpr size_t kFlushThresholdBytes = 64 * 1024;

// A request body block larger than this goes back to the pool once the
// request is answered instead of staying leased to an idle connection.
constexpr size_t kMaxRetainedBodyBytes = 64 * 1024;

// A WebSocket frame with at least this much payload is offered to the
// socket at once, straight from the caller's memory, rather than copied into
// the corked output first (see sendWebSocketFrameRaw).
constexpr size_t kDirectFrameBytes = 8 * 1024;

// The idle timer is refreshed on every read and write; one that would land
// within this much of the deadline already armed is left alone. Timeouts are
// tens of seconds, so firing up to this much early costs nothing, and it
// keeps the timer heap out of the per-request path.
constexpr std::chrono::milliseconds kTimerSlack{1000};

// Assembles a response head in a stack buffer and hands it to the output
// string in one append, instead of a dozen small appends (each a call into
// libstdc++, and a strlen for a literal). A head that outgrows the buffer
// spills to the string as it goes.
class HeadWriter
{
public:
    explicit HeadWriter(std::string& out) : out_(out) {}
    ~HeadWriter() { spill(); }

    void put(std::string_view s)
    {
        if (s.size() > sizeof(buffer_) - used_) {
            spill();
            if (s.size() > sizeof(buffer_)) {
                out_.append(s.data(), s.size());
                return;
            }
        }
        std::memcpy(buffer_ + used_, s.data(), s.size());
        used_ += s.size();
    }

    void put(char c)
    {
        if (used_ == sizeof(buffer_))
            spill();

        buffer_[used_++] = c;
    }

    void putDecimal(size_t value)
    {
        char digits[20];
        size_t count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value);
        while (count)
            put(digits[--count]);
    }

private:
    void spill()
    {
        if (used_) {
            out_.append(buffer_, used_);
            used_ = 0;
        }
    }

    std::string& out_;
    char buffer_[1024];
    size_t used_ = 0;
};

// The host part of a Host header value: no port, IPv6 literals keep their brackets.
std::string_view hostWithoutPort(std::string_view value)
{
    if (!value.empty() && value.front() == '[') {
        const size_t close = value.find(']');
        return close == std::string_view::npos ? value : value.substr(0, close + 1);
    }
    const size_t colon = value.rfind(':');
    return colon == std::string_view::npos ? value : value.substr(0, colon);
}

bool listedIgnoreCase(const std::vector<std::string>& list, std::string_view value)
{
    for (const std::string& item : list)
        if (equalsIgnoreCase(item, value))
            return true;

    return false;
}

// The names in a comma-separated header value that the allowlist contains,
// in the client's spelling.
std::string allowedHeaderNames(std::string_view requested, const std::vector<std::string>& allowed)
{
    std::string result;
    while (!requested.empty()) {
        const size_t comma = requested.find(',');
        std::string_view name = requested.substr(0, comma);
        while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
            name.remove_prefix(1);
        while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
            name.remove_suffix(1);
        if (!name.empty() && listedIgnoreCase(allowed, name)) {
            if (!result.empty())
                result += ", ";

            result.append(name.data(), name.size());
        }
        if (comma == std::string_view::npos)
            break;

        requested.remove_prefix(comma + 1);
    }
    return result;
}
}  // namespace

// ---------------------------------------------------------------------
// Session: owns one accepted connection's whole lifecycle -- HTTP request
// parsing/dispatch/response, keep-alive, and (after an upgrade) WebSocket
// framing. Sessions are recycled: each loop keeps a pool of them (see
// internal::SessionPool), a closed connection's Session goes back to the
// pool, and an accepted socket is attached to an idle one -- so a
// connection costs no Session allocation, and the completion callbacks a
// Session registers capture nothing but `this`, small enough to live inside
// std::function without a heap block of their own.
//
// A Session therefore outlives any single connection, and anything that can
// reach one after its connection is gone -- a handler's `done`, a
// WebSocketConnection a handler kept -- checks a generation first. The pool
// hands a Session out again only from a later loop round than the one it
// was released in: callers up the stack that released it may still be
// unwinding through its members during the current round.
//
// Output is corked: a response is appended to `writeData_` rather than sent
// on the spot, and everything appended is handed to the socket in one write
// once the input at hand has been worked through (flush()). A client that
// pipelines its requests thereby gets all their responses in one send() and
// one segment, and a client that does not pays nothing for it -- one request
// in the buffer, one response, one send(). Only the input already in hand is
// answered in a batch; the Session never waits for more to arrive.
// ---------------------------------------------------------------------
class Session
{
public:
    Session(Server& server, IoLoop& loop, size_t loopIndex)
        : server_(server), loop_(loop), loopIndex_(loopIndex), readBuffer_(server.buffers_)
    {
        request_.body = Buffer(server_.buffers_);
        parser_.setMaxHeaderBytes(server_.config().maxHeaderBytes);
        parser_.setMaxBodyBytes(server_.config().maxBodyBytes);
        parser_.setIncrementalConsumption(true);
        writeData_.reserve(1024);
        pendingWrite_.reserve(1024);
    }

    // Begins serving `connection`; the Session must be idle.
    void attach(std::shared_ptr<internal::TlsStream> connection, std::string peerIp = {})
    {
        connection_ = std::move(connection);
        peerIp_ = std::move(peerIp);
        ++connectionGeneration_;
        closing_ = false;
        responseStarted_ = false;
        handlerPending_ = false;
        zombie_ = false;
        parser_.reset();
        resetRequest();
        resetResponse();
        readBuffer_.clear();
        writeData_.clear();
        pendingWrite_.clear();
        largeBody_.clear();
        writeInFlight_ = false;
        reading_ = false;
        processing_ = false;
        stalled_ = false;
        afterFlush_ = AfterFlush::None;
        upgradeHandler_ = nullptr;
        context_.reset();
        timeoutTimerId_ = 0;
        timerDue_ = {};

        isWebSocket_ = false;
        webSocketConnection_.reset();
        webSocketFrameBytesNeeded_ = 0;
        webSocketCorkedFrames_ = 0;
        webSocketInFlightFrames_ = 0;
        webSocketClosing_ = false;
        webSocketPendingMessage_.clear();
        webSocketPendingOpcode_ = WsOpcode::Text;
        webSocketHasPendingMessage_ = false;

        isStreaming_ = false;
        streamConnection_.reset();
        streamWriteInFlight_ = false;
        streamWriteBuffer_.clear();

        requestDeadline_ = std::chrono::steady_clock::now() +
                           std::chrono::seconds(server_.config().requestTimeoutSeconds);
        requestStarted_ = false;
        served_ = false;
        headerPhase_ = true;
        headerDeadline_ = std::chrono::steady_clock::now() +
                          std::chrono::seconds(server_.config().headerReadTimeoutSeconds);
        resetTimeout();
        readMore();
    }

    bool attached() const { return connection_ != nullptr; }
    void shutdown() { closeAndCleanup(); }

private:
    // What to do once every corked byte has been written.
    enum class AfterFlush
    {
        None,
        Close,    // the last response asked for (or forced) Connection: close
        Upgrade,  // the 101 is out: hand the connection to the WebSocket handler
        Stream    // the head of a streaming response is out: start pushing
    };

    // --- HTTP path ---

    // Where a read lands. The loop's own receive buffer serves whenever what
    // arrives will be consumed at once; bytes known to belong to something
    // still incomplete -- a request body, a WebSocket frame whose start is
    // already in readBuffer_ -- are read straight into place instead, so
    // they are never copied.
    enum class ReadTarget
    {
        LoopBuffer,
        Body,
        ReadBuffer
    };

    void readMore()
    {
        if (closing_ || !server_.running_) {
            closeAndCleanup();
            return;
        }
        if (reading_)
            return;

        reading_ = true;
        if (isWebSocket_) {
            if (!readBuffer_.empty()) {
                const size_t want =
                    std::max(webSocketFrameBytesNeeded_, readBuffer_.size() + 4096);
                readBuffer_.reserve(want);
                connection_->asyncRead(readBuffer_.data() + readBuffer_.size(),
                                       readBuffer_.capacity() - readBuffer_.size(),
                                       [this](IoResult result) {
                                           onReadComplete(result, ReadTarget::ReadBuffer);
                                       });
                return;
            }
        } else if (!isStreaming_) {
            const size_t remaining = parser_.writableBodyBytes(request_);
            if (remaining) {
                connection_->asyncRead(request_.body.data() + request_.body.size(),
                                       std::min(remaining, kReadChunkSize), [this](IoResult result) {
                                           onReadComplete(result, ReadTarget::Body);
                                       });
                return;
            }
        }
        connection_->asyncRead(nullptr, 0, [this](IoResult result) {
            onReadComplete(result, ReadTarget::LoopBuffer);
        });
    }

    void onReadComplete(IoResult result, ReadTarget target)
    {
        reading_ = false;
        if (closing_)
            return;

        if (!result.ok || result.bytes == 0) {
            closeAndCleanup();
            return;
        }
        // A streaming response never expects input beyond the request that
        // opened it; anything arriving here (data or the peer closing) just
        // ends it -- there is no request to parse and nothing to reply to.
        if (isStreaming_) {
            closeAndCleanup();
            return;
        }
        try {
            if (!isWebSocket_ && !requestStarted_) {
                // The first bytes of a request: it has requestTimeoutSeconds
                // from now, however long the connection sat idle before.
                requestStarted_ = true;
                requestDeadline_ = loop_.now() + std::chrono::seconds(
                                                     server_.config().requestTimeoutSeconds);
                if (served_) {
                    headerPhase_ = true;
                    headerDeadline_ = loop_.now() + std::chrono::seconds(
                                                        server_.config().headerReadTimeoutSeconds);
                }
            }
            resetTimeout();
            // Bytes that arrived in the loop's buffer: ours until this
            // callback returns.
            const unsigned char* arrived = result.data;
            if (isWebSocket_) {
                if (target == ReadTarget::ReadBuffer) {
                    readBuffer_.setSize(readBuffer_.size() + result.bytes);
                    processWebSocketInput(readBuffer_.view());
                } else if (readBuffer_.empty()) {
                    processWebSocketInput(
                        std::string_view(reinterpret_cast<const char*>(arrived), result.bytes));
                } else {
                    readBuffer_.append(arrived, result.bytes);
                    processWebSocketInput(readBuffer_.view());
                }
            } else if (target == ReadTarget::Body) {
                parser_.commitBodyBytes(result.bytes, request_);
                processHttp(readBuffer_.view());
            } else if (readBuffer_.empty()) {
                // The common case: parse straight out of the read, with
                // nothing copied; only what the parser leaves over (a
                // partial or pipelined next request) is kept.
                processHttp(std::string_view(reinterpret_cast<const char*>(arrived), result.bytes));
            } else {
                readBuffer_.append(arrived, result.bytes);
                processHttp(readBuffer_.view());
            }
        } catch (...) {
            closeAndCleanup();
        }
    }

    // Works through `input` -- either readBuffer_'s own contents or a fresh
    // read -- answering every complete request in it whose handler answers
    // synchronously, then flushes. Stops early at a request whose response
    // is still to come (its `done` resumes via continueProcessing()), at an
    // upgrade or streaming response, at a response body too large to cork,
    // or when more output is corked than the socket has taken. On return
    // readBuffer_ holds exactly what was not consumed.
    void processHttp(std::string_view input)
    try {
        processing_ = true;
        size_t offset = 0;
        for (;;) {
            if (closing_) {
                processing_ = false;
                return;
            }
            size_t consumed = 0;
            std::string error;
            const ParseStatus status = parser_.parse(input.substr(offset), consumed, request_, error);
            if (status != ParseStatus::NeedMore || !parser_.inHead())
                headerPhase_ = false;

            offset += consumed;
            if (status == ParseStatus::Error) {
                // Whatever followed the malformed request goes unanswered.
                readBuffer_.clear();
                const int errorStatus = parser_.errorStatus();
                if (errorStatus == 503)
                    HTTP_LOG_WARN("request refused: buffer pool exhausted");
                else
                    HTTP_LOG_DEBUG("request rejected: %s", error.c_str());

                // The reason phrase only: the parser's own text stays in the log.
                sendErrorAndClose(errorStatus, reasonPhrase(errorStatus));
                processing_ = false;
                flush();
                return;
            }
            if (status == ParseStatus::NeedMore) {
                keepUnconsumed(input, offset);
                if (parser_.takeExpectContinue()) {
                    // RFC 9110 §10.1.1. Without this a libcurl client posting
                    // an image sits on the body for its full one-second
                    // timeout before every request; measured as exactly that
                    // on /api/alpr.
                    writeData_ += "HTTP/1.1 100 Continue\r\n\r\n";
                }
                processing_ = false;
                flush();
                if (!closing_)
                    readMore();

                return;
            }
            parser_.reset();
            dispatchRequest();
            // A handler that answers later, an upgrade, a stream, a body
            // written uncorked, or a close: nothing further is parsed for now.
            if (!responseStarted_ || afterFlush_ != AfterFlush::None || stalled_ || closing_)
                break;

            if (writeData_.size() >= kFlushThresholdBytes) {
                if (writeInFlight_) {
                    // The socket has not taken the previous batch yet; parsing
                    // on would only pile up more. Resumed by onWriteComplete.
                    stalled_ = true;
                    break;
                }
                flush();
            }
        }
        keepUnconsumed(input, offset);
        processing_ = false;
        flush();
    } catch (...) {
        processing_ = false;
        closeAndCleanup();
    }

    // Picks parsing up again once a response that had stopped it is corked.
    void continueProcessing()
    {
        if (closing_ || processing_ || stalled_ || afterFlush_ != AfterFlush::None)
            return;

        if (!readBuffer_.empty())
            processHttp(readBuffer_.view());
        else
            readMore();
    }

    void keepUnconsumed(std::string_view input, size_t consumed)
    {
        if (input.data() == reinterpret_cast<const char*>(readBuffer_.data())) {
            readBuffer_.consumeFront(consumed);
            return;
        }
        readBuffer_.clear();
        if (consumed < input.size())
            readBuffer_.append(input.data() + consumed, input.size() - consumed);
    }

    void dispatchRequest()
    {
        if (!server_.running_) {
            closeAndCleanup();
            return;
        }
        responseStarted_ = false;
        const uint64_t generation = ++requestGeneration_;
        resetResponse();
        context_.emplace(request_, response_);
        context_->setRemoteAddress(connection_->peerAddress());
        context_->setLoop(&loop_);

        if (!server_.config().allowedHosts.empty() &&
            !listedIgnoreCase(server_.config().allowedHosts,
                              hostWithoutPort(request_.headers.get("Host")))) {
            context_->status(421).text("Misdirected Request");
            sendResponse();
            return;
        }
        // One vector for every match, reused across requests: the context
        // swaps its previous storage back into it (see setRouteParams).
        std::vector<std::pair<std::string, std::string>>& params = routeParams_;
        params.clear();
        if (request_.isUpgrade && request_.upgradeTo == "websocket") {
            const WebSocketHandler* handler =
                server_.webSocketRoutes_.match(HttpMethod::Get, request_.path, params);
            if (handler) {
                upgradeToWebSocket(*handler, std::move(params));
                return;
            }
        }

        if (request_.method == HttpMethod::Get) {
            const StreamHandler* handler = server_.streamRoutes_.match(HttpMethod::Get, request_.path, params);
            if (handler) {
                beginStream(*handler, std::move(params));
                return;
            }
        }

        const Handler* handler = server_.router_.match(request_.method, request_.path, params);
        if (handler) {
            context_->setRouteParams(std::move(params));
            try {
                // `done` may outlive this request -- a handler that answers
                // from a worker keeps it -- and the Session it names may by
                // then serve another connection, or none: the generation
                // check makes a late call a no-op.
                (*handler)(*context_, [this, generation]() {
                    try {
                        if (requestGeneration_ != generation)
                            return;

                        if (handlerPending_) {
                            handlerPending_ = false;
                            if (zombie_) {
                                retire();
                                return;
                            }
                        }
                        if (!responseStarted_ && !closing_)
                            sendResponse();
                    } catch (...) {
                        closeAndCleanup();
                    }
                });
                // A handler that has not answered holds on to the request
                // and the context until it calls `done`.
                handlerPending_ = !responseStarted_;
            } catch (...) {
                if (!responseStarted_)
                    sendErrorAndClose(503, "Service Unavailable");
            }
            return;
        }

        // Explicit routes take priority; static files (e.g. an SPA mounted
        // at "/") are the fallback so a root-mounted static handler can't
        // shadow API routes registered elsewhere.
        for (auto& staticHandler : server_.staticHandlers_) {
            if (staticHandler.tryHandle(*context_)) {
                sendResponse();
                return;
            }
        }

        // The path is known but this method is not: answer OPTIONS ourselves
        // (a browser preflight must not see a 405) and otherwise 405 + Allow.
        std::vector<HttpMethod> allowed = server_.router_.allowedMethods(request_.path);
        if (!allowed.empty()) {
            std::string allow;
            for (HttpMethod m : allowed) {
                if (!allow.empty())
                    allow += ", ";

                allow += methodName(m);
            }
            allow += ", OPTIONS";

            if (request_.method == HttpMethod::Options) {
                context_->status(204);
                response_.setHeader("Allow", allow);
                if (!server_.config().corsAllowOrigin.empty()) {
                    response_.setHeader("Access-Control-Allow-Methods", allow);
                    std::string_view requested =
                        request_.headers.get("Access-Control-Request-Headers");
                    std::string granted =
                        allowedHeaderNames(requested, server_.config().corsAllowedHeaders);
                    response_.setHeader("Access-Control-Allow-Headers",
                                        granted.empty() ? "Content-Type" : std::move(granted));
                    response_.setHeader("Access-Control-Max-Age", "86400");
                }
            } else {
                context_->status(405).text("Method Not Allowed");
                response_.setHeader("Allow", allow);
            }
        } else if (server_.notFound_) {
            context_->status(404);
            server_.notFound_(*context_);
        } else {
            context_->status(404).text("Not Found");
        }
        sendResponse();
    }

    // Corks the response to the current request. Called either from inside
    // processHttp() (a handler that answered before returning) -- then the
    // loop there carries on and flushes -- or later, from a handler's
    // `done`, in which case it flushes and resumes parsing itself.
    void sendResponse()
    {
        if (closing_ || responseStarted_)
            return;

        responseStarted_ = true;
        const bool keepAlive = request_.keepAlive && server_.running_;
        const bool statusAllowsBody =
            response_.status >= 200 && response_.status != 204 && response_.status != 304;
        const bool hasBody = request_.method != HttpMethod::Head && statusAllowsBody;

        serializeHead(response_, keepAlive, statusAllowsBody);
        if (hasBody && !response_.body.empty()) {
            if (response_.body.size() <= kMaxCoalescedBodyBytes) {
                writeData_ += response_.body;
            } else {
                // Written straight from the handler's own string, after the
                // corked bytes, rather than copied; parsing waits for it.
                largeBody_.swap(response_.body);
                stalled_ = true;
            }
        }
        if (!keepAlive)
            afterFlush_ = AfterFlush::Close;

        // The handler is finished with the request once it has answered;
        // the next one on the connection may be parsed right away.
        resetRequest();
        // Back to idle: the next request starts its own deadline.
        requestStarted_ = false;
        served_ = true;
        requestDeadline_ =
            loop_.now() + std::chrono::seconds(server_.config().requestTimeoutSeconds);
        if (processing_)
            return;

        flush();
        continueProcessing();
    }

    // Hands everything corked to the socket, unless a write is still in
    // flight -- then it goes with the next flush, from onWriteComplete().
    void flush()
    {
        if (writeInFlight_ || closing_)
            return;

        if (writeData_.empty()) {
            if (!largeBody_.empty())
                writeLargeBody();

            return;
        }
        writeInFlight_ = true;
        webSocketInFlightFrames_ = webSocketCorkedFrames_;
        webSocketCorkedFrames_ = 0;
        // The two buffers trade places, so each keeps the capacity it grew.
        pendingWrite_.swap(writeData_);
        writeData_.clear();
        connection_->asyncWrite(reinterpret_cast<const unsigned char*>(pendingWrite_.data()),
                                pendingWrite_.size(),
                                [this](IoResult result) { onWriteComplete(result); });
    }

    void writeLargeBody()
    {
        writeInFlight_ = true;
        connection_->asyncWrite(reinterpret_cast<const unsigned char*>(largeBody_.data()),
                                largeBody_.size(), [this](IoResult result) {
                                    largeBody_.clear();
                                    onWriteComplete(result);
                                });
    }

    void onWriteComplete(IoResult result)
    {
        writeInFlight_ = false;
        if (closing_)
            return;

        if (!result.ok) {
            closeAndCleanup();
            return;
        }
        resetTimeout();
        webSocketInFlightFrames_ = 0;
        if (!writeData_.empty()) {
            flush();
            return;
        }
        if (isWebSocket_) {
            // Our Close frame is on the wire (the peer's, if it sent one
            // first, has been read): the handshake is done.
            if (webSocketClosing_)
                closeAndCleanup();

            return;
        }
        if (!largeBody_.empty()) {
            writeLargeBody();
            return;
        }
        // Everything corked so far is on the wire.
        switch (afterFlush_) {
            case AfterFlush::None: break;
            case AfterFlush::Close: closeAndCleanup(); return;
            case AfterFlush::Upgrade: afterFlush_ = AfterFlush::None; completeUpgrade(); return;
            case AfterFlush::Stream:
                afterFlush_ = AfterFlush::None;
                isStreaming_ = true;
                readMore();
                return;
        }
        if (stalled_) {
            stalled_ = false;
            continueProcessing();
        }
    }

    void sendErrorAndClose(int status, std::string_view message)
    {
        if (closing_ || responseStarted_) {
            closeAndCleanup();
            return;
        }
        responseStarted_ = true;
        resetResponse();
        response_.status = status;
        response_.setHeader("Content-Type", "text/plain; charset=utf-8");
        response_.setHeader("Connection", "close");
        if (status == 503)
            response_.setHeader("Retry-After", "1");

        response_.body.assign(message.data(), message.size());
        request_.keepAlive = false;
        serializeHead(response_, false, true);
        writeData_ += response_.body;
        afterFlush_ = AfterFlush::Close;
        if (!processing_)
            flush();
    }

    // --- WebSocket path ---

    void upgradeToWebSocket(const WebSocketHandler& handler,
                            std::vector<std::pair<std::string, std::string>> params)
    {
        std::string_view key = request_.headers.get("Sec-WebSocket-Key");
        const auto decodedKey = base64Decode(key);
        if (request_.method != HttpMethod::Get || request_.versionMinor != 1 ||
            !request_.body.empty() || request_.headers.get("Sec-WebSocket-Version") != "13" ||
            decodedKey.size() != 16 ||
            base64Encode(reinterpret_cast<const uint8_t*>(decodedKey.data()), decodedKey.size()) !=
                key) {
            context_->status(400).text("Bad Request: invalid WebSocket handshake");
            sendResponse();
            return;
        }
        const std::string_view origin = request_.headers.get("Origin");
        if (!server_.config().allowedOrigins.empty() && !origin.empty() &&
            !listedIgnoreCase(server_.config().allowedOrigins, origin)) {
            context_->status(403).text("Forbidden: origin not allowed");
            sendResponse();
            return;
        }
        std::string accept = computeWebSocketAccept(key);

        writeData_ += "HTTP/1.1 101 Switching Protocols\r\n";
        writeData_ += "Upgrade: websocket\r\n";
        writeData_ += "Connection: Upgrade\r\n";
        writeData_ += "Sec-WebSocket-Accept: ";
        writeData_ += accept;
        writeData_ += "\r\n\r\n";

        webSocketConnection_ = std::make_shared<WebSocketConnection>();
        webSocketConnection_->setRouteParams(std::move(params));
        webSocketConnection_->setRemoteAddress(context_->remoteAddress());
        // The handle a handler keeps may outlive this connection; a call
        // through it after that -- or after this Session moved on to another
        // connection -- must do nothing.
        const uint64_t generation = connectionGeneration_;
        webSocketConnection_->bindSendFunction([this, generation](std::string_view data, bool binary) {
            if (connectionGeneration_ == generation && !closing_)
                sendWebSocketFrame(data, binary);
        });
        webSocketConnection_->bindCloseFunction([this, generation]() {
            if (connectionGeneration_ == generation && !closing_)
                closeWebSocketConnection();
        });
        webSocketConnection_->bindPostFunction([this, generation](std::function<void()> task) {
            return connectionGeneration_ == generation && !closing_ ? loop_.tryPost(std::move(task))
                                                                     : false;
        });

        upgradeHandler_ = &handler;
        afterFlush_ = AfterFlush::Upgrade;
        if (!processing_)
            flush();
    }

    // The 101 (and any responses corked before it) is on the wire.
    void completeUpgrade()
    {
        isWebSocket_ = true;
        const WebSocketHandler* handler = upgradeHandler_;
        upgradeHandler_ = nullptr;
        try {
            (*handler)(webSocketConnection_);
        } catch (...) {
            closeAndCleanup();
            return;
        }
        if (closing_)
            return;

        resetTimeout();
        // Frames the client sent on the heels of its handshake are already
        // waiting in readBuffer_.
        if (!readBuffer_.empty())
            processWebSocketInput(readBuffer_.view());
        else
            readMore();
    }

    // --- streaming (server-push) path ---

    // The handler runs synchronously, right here: it configures the
    // response on `context_` and keeps `streamConnection_` for later
    // writes, exactly like a WebSocket route's handler keeps its
    // connection. Nothing is sent to the client until this call returns,
    // and nothing the handler pushes goes out before the head has.
    void beginStream(const StreamHandler& handler, std::vector<std::pair<std::string, std::string>> params)
    {
        context_->setRouteParams(std::move(params));
        streamConnection_ = std::make_shared<StreamingConnection>();
        const uint64_t generation = connectionGeneration_;
        streamConnection_->bindWriteFunction([this, generation](const void* data, size_t length) {
            return connectionGeneration_ == generation && !closing_ ? writeStreamChunk(data, length)
                                                                     : false;
        });
        streamConnection_->bindCloseFunction([this, generation]() {
            if (connectionGeneration_ == generation && !closing_)
                closeAndCleanup();
        });
        streamConnection_->bindPostFunction([this, generation](std::function<void()> task) {
            return connectionGeneration_ == generation && !closing_ ? loop_.tryPost(std::move(task))
                                                                     : false;
        });

        try {
            handler(*context_, streamConnection_);
        } catch (...) {
            closeAndCleanup();
            return;
        }
        if (closing_)
            return;

        // No Content-Length and never chunked: the body is simply
        // everything written until the connection closes (RFC 7230
        // §3.3.3 #7), which is what a multipart/x-mixed-replace viewer
        // expects. Never kept alive for a further request on the same
        // connection -- there is no defined end to hand back to the
        // ordinary request/response loop.
        response_.setHeader("Connection", "close");
        serializeHead(response_, false, false);
        afterFlush_ = AfterFlush::Stream;
        if (!processing_)
            flush();
    }

    // A previous write still in flight loses the new frame rather than
    // queuing it, matching /ws/alpr's own "a live frame that waits is
    // worse than one that never ran": a stale frame behind a slow
    // reader is not worth delivering once a fresher one exists.
    bool writeStreamChunk(const void* data, size_t length)
    {
        if (closing_ || !isStreaming_ || streamWriteInFlight_)
            return false;

        streamWriteInFlight_ = true;
        streamWriteBuffer_.assign(reinterpret_cast<const char*>(data), length);
        connection_->asyncWrite(reinterpret_cast<const unsigned char*>(streamWriteBuffer_.data()),
                                streamWriteBuffer_.size(), [this](IoResult r) {
                                    streamWriteInFlight_ = false;
                                    if (!r.ok) {
                                        closeAndCleanup();
                                        return;
                                    }
                                    resetTimeout();
                                });
        return true;
    }

    // Works through every complete frame in `input` -- either readBuffer_'s
    // own contents or a fresh read -- unmasking each in place and handing
    // its payload to the handler as a view into that buffer (valid for the
    // duration of the call). Frames the handler sends meanwhile are corked
    // and go out in one write at the end. On return readBuffer_ holds
    // exactly the bytes of a frame still incomplete.
    void processWebSocketInput(std::string_view input)
    {
        processing_ = true;
        size_t offset = 0;
        bool readOn = true;
        // readBuffer_ is the Session's own and the loop's receive buffer is
        // ours until this returns; unmasking rewrites either in place.
        char* base = const_cast<char*>(input.data());
        for (;;) {
            if (closing_) {
                processing_ = false;
                return;
            }
            const std::string_view rest = input.substr(offset);
            WsFrameHeader hdr;
            try {
                if (!tryParseFrameHeader(rest, hdr))
                    break;
            } catch (const std::invalid_argument&) {
                closeWebSocketConnection();
                readOn = false;
                break;
            }
            if (!hdr.masked || hdr.payloadLen > server_.config().maxWebSocketMessageBytes) {
                closeWebSocketConnection();
                readOn = false;
                break;
            }
            const size_t payloadLen = static_cast<size_t>(hdr.payloadLen);
            const size_t totalNeeded = hdr.headerSize + payloadLen;
            if (rest.size() < totalNeeded) {
                // The rest of this frame is read straight into readBuffer_
                // behind what is here (see readMore).
                webSocketFrameBytesNeeded_ = totalNeeded;
                break;
            }

            char* payloadData = base + offset + hdr.headerSize;
            applyMask(payloadData, payloadLen, hdr.maskKey);
            offset += totalNeeded;
            if (!handleWebSocketFrame(hdr, std::string_view(payloadData, payloadLen))) {
                readOn = false;
                break;
            }
        }
        if (!closing_) {
            keepUnconsumed(input, offset);
            if (readBuffer_.empty()) {
                webSocketFrameBytesNeeded_ = 0;
                // A block grown for one big frame goes back to the pool
                // rather than staying leased to this connection.
                if (readBuffer_.capacity() > kMaxRetainedBodyBytes)
                    readBuffer_ = Buffer(server_.buffers_);
            }
            resetTimeout();
        }
        processing_ = false;
        flush();
        if (!closing_ && readOn && !webSocketClosing_)
            readMore();
    }

    // One unmasked frame. Returns false once no further frame should be
    // taken from the input: the connection is closing, or in the closing
    // handshake.
    bool handleWebSocketFrame(const WsFrameHeader& hdr, std::string_view payload)
    {
        switch (hdr.opcode) {
            case WsOpcode::Text:
            case WsOpcode::Binary:
                if (webSocketHasPendingMessage_) {
                    closeWebSocketConnection();
                    return false;
                }
                if (hdr.opcode == WsOpcode::Text && hdr.fin && !isValidUtf8(payload)) {
                    closeAndCleanup();
                    return false;
                }
                if (!hdr.fin) {
                    webSocketHasPendingMessage_ = true;
                    webSocketPendingOpcode_ = hdr.opcode;
                    webSocketPendingMessage_.assign(payload.data(), payload.size());
                } else if (webSocketConnection_->messageCallback()) {
                    webSocketConnection_->messageCallback()(payload, hdr.opcode == WsOpcode::Binary);
                }
                break;
            case WsOpcode::Continuation:
                if (!webSocketHasPendingMessage_) {
                    closeWebSocketConnection();
                    return false;
                }
                if (payload.size() > server_.config().maxWebSocketMessageBytes -
                                         webSocketPendingMessage_.size()) {
                    closeWebSocketConnection();
                    return false;
                }
                webSocketPendingMessage_.append(payload.data(), payload.size());
                if (hdr.fin) {
                    webSocketHasPendingMessage_ = false;
                    if (webSocketPendingOpcode_ == WsOpcode::Text &&
                        !isValidUtf8(webSocketPendingMessage_)) {
                        closeAndCleanup();
                        return false;
                    }
                    if (webSocketConnection_->messageCallback()) {
                        webSocketConnection_->messageCallback()(
                            webSocketPendingMessage_, webSocketPendingOpcode_ == WsOpcode::Binary);
                    }
                    webSocketPendingMessage_.clear();
                }
                break;
            case WsOpcode::Ping: sendWebSocketFrameRaw(payload, WsOpcode::Pong); break;
            case WsOpcode::Pong: break;
            case WsOpcode::Close:
                if (payload.size() == 1 ||
                    (payload.size() >= 2 && !isValidUtf8(payload.substr(2)))) {
                    closeAndCleanup();
                    return false;
                }
                if (payload.size() >= 2) {
                    const unsigned code = static_cast<unsigned char>(payload[0]) * 256u +
                                          static_cast<unsigned char>(payload[1]);
                    if (code < 1000 || code >= 5000 || code == 1004 || code == 1005 ||
                        code == 1006 || code == 1015 || (code >= 1016 && code < 3000)) {
                        closeAndCleanup();
                        return false;
                    }
                }
                if (webSocketClosing_) {
                    closeAndCleanup();
                } else {
                    webSocketClosing_ = true;
                    sendWebSocketFrameRaw(payload, WsOpcode::Close);
                }
                return false;
            default: closeWebSocketConnection(); return false;
        }
        return !closing_;
    }

    void sendWebSocketFrame(std::string_view data, bool binary)
    {
        if (webSocketClosing_)
            return;

        if (!binary && !isValidUtf8(data)) {
            closeAndCleanup();
            return;
        }
        sendWebSocketFrameRaw(data, binary ? WsOpcode::Binary : WsOpcode::Text);
    }

    // Corks the frame; it goes out with the next flush -- at once, unless
    // frames are being handled (then together with whatever those send) or
    // a write is already in flight (then right behind it).
    void sendWebSocketFrameRaw(std::string_view payload, WsOpcode opcode)
    {
        if (closing_)
            return;

        const size_t queuedBytes = writeData_.size() + (writeInFlight_ ? pendingWrite_.size() : 0);
        if (payload.size() > server_.config().maxWebSocketMessageBytes ||
            webSocketCorkedFrames_ + webSocketInFlightFrames_ >=
                server_.config().maxWebSocketQueuedFrames ||
            payload.size() + 10 > server_.config().maxWebSocketQueuedBytes - queuedBytes) {
            closeAndCleanup();
            return;
        }
        if (payload.size() >= kDirectFrameBytes && !writeInFlight_ && writeData_.empty()) {
            // Big enough that copying it is worth a syscall to avoid: header
            // and payload go to the socket now, and only what it did not take
            // is copied and queued. The payload may live in the receive
            // buffer, which the next read overwrites -- hence now or never.
            char header[10];
            const size_t headerSize = encodeFrameHeader(opcode, payload.size(), true, header);
            const size_t sent = connection_->tryWrite(
                reinterpret_cast<const unsigned char*>(header), headerSize,
                reinterpret_cast<const unsigned char*>(payload.data()), payload.size());
            if (sent >= headerSize + payload.size()) {
                resetTimeout();
                return;
            }
            if (sent < headerSize)
                writeData_.append(header + sent, headerSize - sent);

            writeData_.append(payload.data() + (sent > headerSize ? sent - headerSize : 0),
                              payload.size() - (sent > headerSize ? sent - headerSize : 0));
        } else {
            appendFrame(opcode, payload, true, writeData_);
        }
        ++webSocketCorkedFrames_;
        if (!processing_)
            flush();
    }

    void closeWebSocketConnection()
    {
        if (closing_ || webSocketClosing_)
            return;

        webSocketClosing_ = true;
        sendWebSocketFrameRaw(std::string_view("\x03\xe8", 2), WsOpcode::Close);
        if (!closing_ && timeoutTimerId_) {
            loop_.refreshTimer(timeoutTimerId_, std::chrono::seconds(5));
            timerDue_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }
    }

    // --- shared ---

    void resetRequest()
    {
        request_.method = HttpMethod::Unknown;
        request_.path = {};
        request_.rawTarget = {};
        request_.queryString = {};
        request_.head.clear();
        request_.decodedPath.clear();
        request_.versionMajor = 1;
        request_.versionMinor = 1;
        request_.headers.clear();
        if (request_.body.capacity() > kMaxRetainedBodyBytes)
            request_.body = Buffer(server_.buffers_);
        else
            request_.body.clear();

        request_.keepAlive = true;
        request_.isUpgrade = false;
        request_.upgradeTo = {};
    }

    void resetResponse()
    {
        response_.status = 200;
        response_.headers.clear();
        response_.body.clear();
    }

    // Status line and headers, appended to writeData_. The headers the
    // server adds itself -- Content-Length (when `withContentLength` and the
    // handler set none), Connection, Server, CORS -- are written straight
    // into the head rather than first inserted into `response.headers` and
    // looked up again, all in one pass over the handler's own headers.
    void serializeHead(const HttpResponse& response, bool keepAlive, bool withContentLength)
    {
        HeadWriter head(writeData_);
        head.put("HTTP/1.1 ");
        head.putDecimal(static_cast<size_t>(response.status));
        head.put(' ');
        head.put(reasonPhrase(response.status));
        head.put("\r\n");

        bool hasContentLength = false;
        bool hasConnection = false;
        bool hasServer = false;
        bool hasAllowOrigin = false;
        for (auto& h : response.headers.items()) {
            switch (h.first.size()) {
                case 6: hasServer = hasServer || equalsIgnoreCase(h.first, "Server"); break;
                case 10: hasConnection = hasConnection || equalsIgnoreCase(h.first, "Connection"); break;
                case 14:
                    hasContentLength = hasContentLength || equalsIgnoreCase(h.first, "Content-Length");
                    break;
                case 27:
                    hasAllowOrigin =
                        hasAllowOrigin || equalsIgnoreCase(h.first, "Access-Control-Allow-Origin");
                    break;
                default: break;
            }
            head.put(h.first);
            head.put(": ");
            head.put(h.second);
            head.put("\r\n");
        }
        if (withContentLength && !hasContentLength) {
            head.put("Content-Length: ");
            head.putDecimal(response.body.size());
            head.put("\r\n");
        }
        if (!hasConnection && !hasServer && !hasAllowOrigin) {
            // The ordinary case: the fixed tail, assembled once per server.
            head.put(keepAlive ? server_.headTailKeepAlive_ : server_.headTailClose_);
            return;
        }
        if (!hasConnection)
            head.put(keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n");

        if (!hasServer) {
            head.put("Server: ");
            head.put(server_.config().serverHeader);
            head.put("\r\n");
        }
        const std::string& corsOrigin = server_.config().corsAllowOrigin;
        if (!corsOrigin.empty() && !hasAllowOrigin) {
            head.put("Access-Control-Allow-Origin: ");
            head.put(corsOrigin);
            head.put("\r\n");
            // A named origin makes the response origin-dependent; "*" does not.
            if (corsOrigin != "*")
                head.put("Vary: Origin\r\n");
        }
        head.put("\r\n");
    }

    void resetTimeout()
    {
        // The round's cached clock is plenty: the timer has a second of slack.
        const auto now = loop_.now();
        auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::seconds(server_.config().keepAliveTimeoutSeconds));
        if (!isWebSocket_ && !isStreaming_ && (requestStarted_ || !served_)) {
            delay = std::min(delay, std::chrono::duration_cast<std::chrono::milliseconds>(
                                        requestDeadline_ - now));
            if (headerPhase_ && server_.config().headerReadTimeoutSeconds > 0)
                delay = std::min(delay, std::chrono::duration_cast<std::chrono::milliseconds>(
                                            headerDeadline_ - now));

            delay = std::max(delay, std::chrono::milliseconds(0));
        }

        const auto due = now + delay;
        if (timeoutTimerId_ != 0) {
            // Refreshed on every read and write, so almost always to within
            // microseconds of where it already is.
            if (due >= timerDue_ && due - timerDue_ < kTimerSlack)
                return;

            if (loop_.refreshTimer(timeoutTimerId_, delay)) {
                timerDue_ = due;
                return;
            }
        }
        timeoutTimerId_ = loop_.addTimer(delay, [this]() { closeAndCleanup(); });
        timerDue_ = due;
    }

    void closeAndCleanup()
    {
        if (closing_)
            return;

        closing_ = true;
        if (timeoutTimerId_ != 0) {
            loop_.cancelTimer(timeoutTimerId_);
            timeoutTimerId_ = 0;
        }
        // Fires for a clean Close-frame handshake as well as an abnormal
        // drop (timeout, TCP reset) -- either way the handler needs to
        // know the connection is gone exactly once.
        if (webSocketConnection_ && webSocketConnection_->closeCallback()) {
            try {
                webSocketConnection_->closeCallback()();
            } catch (...) {}
        }
        if (streamConnection_ && streamConnection_->closeCallback()) {
            try {
                streamConnection_->closeCallback()();
            } catch (...) {}
        }
        if (webSocketConnection_)
            webSocketConnection_->releaseCallbacks();

        if (streamConnection_)
            streamConnection_->releaseCallbacks();

        if (!connection_)
            return;

        connection_->close();
        connection_.reset();
        server_.connectionClosed(loopIndex_, peerIp_);
        webSocketConnection_.reset();
        streamConnection_.reset();
        if (handlerPending_) {
            // A handler still holds the request, its body and the context:
            // the Session stays out of the pool until it calls `done` (or
            // the grace period ends, for one that never will).
            zombie_ = true;
            try {
                const auto grace = std::max(60, 2 * server_.config().requestTimeoutSeconds);
                zombieTimerId_ = loop_.addTimer(std::chrono::seconds(grace), [this]() {
                    zombieTimerId_ = 0;
                    handlerPending_ = false;
                    if (zombie_)
                        retire();
                });
            } catch (...) {
                handlerPending_ = false;
                retire();
            }
            return;
        }
        retire();
    }

    // The connection is gone and nothing refers to the request any more.
    void retire()
    {
        zombie_ = false;
        if (zombieTimerId_ != 0) {
            loop_.cancelTimer(zombieTimerId_);
            zombieTimerId_ = 0;
        }
        context_.reset();
        server_.releaseSession(loopIndex_, *this);
    }

    Server& server_;
    IoLoop& loop_;
    const size_t loopIndex_;
    std::shared_ptr<internal::TlsStream> connection_;
    // Bumped per attach(): tells a stale WebSocket/stream handle from a live one.
    uint64_t connectionGeneration_ = 0;

    Buffer readBuffer_;
    HttpParser parser_;
    HttpRequest request_;
    HttpResponse response_;
    std::optional<HttpContext> context_;
    std::vector<std::pair<std::string, std::string>> routeParams_;
    std::string writeData_;     // corked output, not yet handed to the socket
    std::string pendingWrite_;  // the batch the socket is taking right now
    std::string largeBody_;     // a body written after pendingWrite_ without being copied
    bool writeInFlight_ = false;
    bool reading_ = false;
    bool processing_ = false;   // inside processHttp()'s loop
    bool stalled_ = false;      // parsing suspended until the socket catches up
    AfterFlush afterFlush_ = AfterFlush::None;
    const WebSocketHandler* upgradeHandler_ = nullptr;
    uint64_t timeoutTimerId_ = 0;
    std::chrono::steady_clock::time_point timerDue_{};
    std::chrono::steady_clock::time_point requestDeadline_;
    bool requestStarted_ = false;  // bytes of a request have arrived and it is not yet answered
    bool served_ = false;          // a response has been corked on this connection
    bool headerPhase_ = false;     // the request line and headers are not complete yet
    std::chrono::steady_clock::time_point headerDeadline_;
    std::string peerIp_;           // key in SessionPool::perIp, empty when not counted
    bool closing_ = true;
    bool responseStarted_ = false;
    uint64_t requestGeneration_ = 0;
    bool handlerPending_ = false;  // a handler returned without answering and has not called done
    bool zombie_ = false;          // connection closed, Session kept for the pending handler
    uint64_t zombieTimerId_ = 0;

    bool isWebSocket_ = false;
    std::shared_ptr<WebSocketConnection> webSocketConnection_;
    size_t webSocketFrameBytesNeeded_ = 0;  // full size of the frame readBuffer_ holds the start of
    size_t webSocketCorkedFrames_ = 0;    // frames in writeData_
    size_t webSocketInFlightFrames_ = 0;  // frames in pendingWrite_
    bool webSocketClosing_ = false;
    std::string webSocketPendingMessage_;
    WsOpcode webSocketPendingOpcode_ = WsOpcode::Text;
    bool webSocketHasPendingMessage_ = false;

    bool isStreaming_ = false;
    std::shared_ptr<StreamingConnection> streamConnection_;
    bool streamWriteInFlight_ = false;
    std::string streamWriteBuffer_;
};

// Sessions of one loop. `idle` is FIFO with the loop round each entry was
// released in, so an accept in the same round takes a different one (see
// the Session comment).
struct internal::SessionPool
{
    std::vector<std::unique_ptr<Session>> all;
    std::deque<std::pair<Session*, uint64_t>> idle;
    size_t capacity = 0;
    std::unordered_map<std::string, size_t> perIp;
};

// ---------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------

Server::Server(ServerConfig config) : config_(std::move(config))
{
    if ((!config_.httpEnabled && !config_.tls) ||
        (config_.tls && config_.httpEnabled && config_.port == config_.tls->port))
        throw std::invalid_argument(
            "HTTP and HTTPS require distinct ports and at least one enabled listener");

    if (config_.tls) {
        tls_ = std::make_unique<internal::ServerTls>();
        tls_->context =
            internal::makeTlsContext(config_.tls->certificateFile, config_.tls->privateKeyFile);
    }
    if (!config_.maxConnectionsPerLoop || !config_.workerQueueCapacity ||
        !config_.reactorTaskCapacity ||
        config_.reactorTimerCapacity < config_.maxConnectionsPerLoop ||
        config_.maxConnectionsPerLoop > 0xFFFFFFu ||
        config_.maxBodyBytes > BufferPool::kSizeClasses[BufferPool::kNumClasses - 1] ||
        config_.maxWebSocketMessageBytes > 16 * 1024 * 1024 ||
        config_.maxWebSocketQueuedBytes < 10 || config_.headerReadTimeoutSeconds < 0 ||
        !isHeaderValue(config_.serverHeader) || !isHeaderValue(config_.corsAllowOrigin)) {
        throw std::invalid_argument("invalid server resource limits");
    }
    if (config_.ioThreads == 0) {
        unsigned hw = std::thread::hardware_concurrency();
        config_.ioThreads = hw > 0 ? hw : 4;
    }
    buffers_.configure(config_.buffers);

    std::string tail = "Server: " + config_.serverHeader + "\r\n";
    if (!config_.corsAllowOrigin.empty()) {
        tail += "Access-Control-Allow-Origin: " + config_.corsAllowOrigin + "\r\n";
        // A named origin makes the response origin-dependent; "*" does not.
        if (config_.corsAllowOrigin != "*")
            tail += "Vary: Origin\r\n";
    }
    tail += "\r\n";
    headTailKeepAlive_ = "Connection: keep-alive\r\n" + tail;
    headTailClose_ = "Connection: close\r\n" + tail;
}

Server::~Server()
{
    stop();
}

Server& Server::onStaticFiles(std::string urlPrefix, std::string rootDir)
{
    return onStaticFiles(std::move(urlPrefix), std::move(rootDir), StaticFileHandler::Options{});
}

Server& Server::onStaticFiles(std::string urlPrefix, std::string rootDir,
                              StaticFileHandler::Options options)
{
    staticHandlers_.emplace_back(std::move(urlPrefix), std::move(rootDir), std::move(options));
    return *this;
}

Server& Server::onWebSocket(std::string pattern, WebSocketHandler handler)
{
    webSocketRoutes_.add(HttpMethod::Get, std::move(pattern), std::move(handler));
    return *this;
}

Server& Server::onStream(std::string pattern, StreamHandler handler)
{
    streamRoutes_.add(HttpMethod::Get, std::move(pattern), std::move(handler));
    return *this;
}

ThreadPool& Server::pool()
{
    std::lock_guard<std::mutex> lock(poolMutex_);
    if (!pool_) {
        unsigned n = config_.workerThreads > 0 ? config_.workerThreads : 2;
        pool_ = std::make_unique<ThreadPool>(n, config_.workerQueueCapacity);
    }
    return *pool_;
}

void Server::releaseSession(size_t index, Session& session)
{
    sessionPools_[index]->idle.emplace_back(&session, loops_[index]->iteration());
}

void Server::connectionClosed(size_t index, const std::string& peerIp)
{
    if (peerIp.empty())
        return;

    auto& perIp = sessionPools_[index]->perIp;
    const auto found = perIp.find(peerIp);
    if (found != perIp.end() && --found->second == 0)
        perIp.erase(found);
}

namespace {
// Tells a connection that is being turned away why, without waiting: one
// non-blocking send into an empty socket buffer, which a fresh connection
// always has room for. TLS connections are just dropped (no handshake done).
void rejectBusy(socket_t fd, bool tls)
{
#ifdef MSG_NOSIGNAL
    if (tls)
        return;

    static const char reply[] =
        "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nRetry-After: 1\r\n"
        "Content-Length: 0\r\n\r\n";
    (void)::send(fd, reply, sizeof(reply) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
#else
    (void)fd;
    (void)tls;
#endif
}
}  // namespace

void Server::handleAccept(size_t index, socket_t fd, bool tls)
{
    if (!running_) {
        closeSocket(fd);
        return;
    }
    internal::SessionPool& pool = *sessionPools_[index];
    Session* session = nullptr;
    if (!pool.idle.empty() && pool.idle.front().second < loops_[index]->iteration()) {
        session = pool.idle.front().first;
        pool.idle.pop_front();
    } else if (pool.all.size() < pool.capacity) {
        try {
            pool.all.push_back(std::make_unique<Session>(*this, *loops_[index], index));
        } catch (...) {
            closeSocket(fd);
            return;
        }
        session = pool.all.back().get();
    } else {
        rejectBusy(fd, tls);
        closeSocket(fd);
        return;
    }

    std::shared_ptr<internal::TlsStream> stream;
    std::string peerIp;
    try {
        auto tcp = std::make_shared<TcpConnection>(*loops_[index], fd);
        if (config_.maxConnectionsPerIp) {
            // "host:port": the host is everything before the last colon.
            const std::string& peer = tcp->peerAddress();
            peerIp = peer.substr(0, peer.rfind(':'));
            size_t& count = pool.perIp[peerIp];
            if (count >= config_.maxConnectionsPerIp) {
                peerIp.clear();
                rejectBusy(tcp->nativeHandle(), tls);
                tcp->close();
                pool.idle.emplace_back(session, loops_[index]->iteration());
                return;
            }
            ++count;
        }
        stream = std::make_shared<internal::TlsStream>(std::move(tcp),
                                                       tls ? tls_->context : internal::TlsContext{});
    } catch (...) {
        // TcpConnection owns (and closed) the descriptor once constructed;
        // before that it is still ours.
        if (!stream)
            closeSocket(fd);

        connectionClosed(index, peerIp);
        pool.idle.emplace_back(session, loops_[index]->iteration());
        return;
    }
    try {
        session->attach(std::move(stream), std::move(peerIp));
    } catch (...) {
        session->shutdown();
    }
}

void Server::runLoop(size_t index)
{
    loops_[index]->run();
}

void Server::run(std::function<bool()> shouldStop)
{
    if (!loops_.empty())
        throw std::logic_error("Server::run may only be called once");

    pool();
    running_.store(true, std::memory_order_relaxed);
    loops_.resize(config_.ioThreads);
    listeners_.resize(config_.ioThreads);
    tlsListeners_.resize(config_.ioThreads);
    sessionPools_.resize(config_.ioThreads);
    try {
        for (size_t i = 0; i < config_.ioThreads; ++i) {
            loops_[i] =
                std::make_unique<IoLoop>(config_.reactorTaskCapacity, config_.reactorTimerCapacity);
            sessionPools_[i] = std::make_unique<internal::SessionPool>();
            sessionPools_[i]->capacity = config_.maxConnectionsPerLoop;
            if (config_.httpEnabled) {
                listeners_[i] = std::make_unique<TcpListener>(*loops_[i], config_.host,
                                                              config_.port, config_.reusePort);
                listeners_[i]->asyncAccept([this, i](socket_t fd) { handleAccept(i, fd); });
            }
            if (config_.tls) {
                tlsListeners_[i] = std::make_unique<TcpListener>(
                    *loops_[i], config_.host, config_.tls->port, config_.reusePort);
                tlsListeners_[i]->asyncAccept(
                    [this, i](socket_t fd) { handleAccept(i, fd, true); });
            }
        }
    } catch (...) {
        running_ = false;
        listeners_.clear();
        tlsListeners_.clear();
        throw;
    }

    loopsReady_.store(true, std::memory_order_release);
    const size_t workers = config_.workerThreads ? config_.workerThreads : 2;
    if (config_.reactorTaskCapacity < config_.workerQueueCapacity + workers)
        HTTP_LOG_WARN(
            "reactorTaskCapacity (%zu) is below workerQueueCapacity + workers (%zu): completions "
            "posted back to a loop can be refused",
            config_.reactorTaskCapacity, config_.workerQueueCapacity + workers);

    if (config_.httpEnabled)
        HTTP_LOG_INFO("HTTP listening on %s:%u with %u I/O thread(s)", config_.host.c_str(),
                      config_.port, config_.ioThreads);

    if (config_.tls)
        HTTP_LOG_INFO("HTTPS listening on %s:%u with %u I/O thread(s)", config_.host.c_str(),
                      config_.tls->port, config_.ioThreads);

    try {
        for (size_t index = 0; index < config_.ioThreads; ++index)
            threads_.emplace_back([this, index] { runLoop(index); });
    } catch (...) {
        for (size_t index = 0; index < threads_.size(); ++index)
            loops_[index]->postControl([this, index] { loops_[index]->stop(); });
        for (auto& thread : threads_)
            thread.join();

        running_ = false;
        throw;
    }
    std::exception_ptr controlFailure;
    {
        std::unique_lock<std::mutex> lock(stopMutex_);
        while (running_) {
            stopCv_.wait_for(lock, std::chrono::milliseconds(100));
            try {
                if (shouldStop && shouldStop())
                    running_ = false;
            } catch (...) {
                controlFailure = std::current_exception();
                running_ = false;
            }
        }
    }
    for (size_t index = 0; index < loops_.size(); ++index) {
        loops_[index]->postControl([this, index] {
            if (listeners_[index])
                listeners_[index]->close();

            if (tlsListeners_[index])
                tlsListeners_[index]->close();
            {
                std::lock_guard<std::mutex> lock(stopMutex_);
                ++closedListeners_;
            }
            stopCv_.notify_all();
        });
    }
    {
        std::unique_lock<std::mutex> lock(stopMutex_);
        stopCv_.wait(lock, [this] { return closedListeners_ == loops_.size(); });
    }
    pool_->shutdown();
    for (size_t index = 0; index < loops_.size(); ++index) {
        loops_[index]->postControl([this, index] {
            for (auto& session : sessionPools_[index]->all)
                session->shutdown();

            loops_[index]->stop();
        });
    }
    for (auto& t : threads_) {
        if (t.joinable())
            t.join();
    }
    // The pools -- and with them every buffer block a Session still held --
    // go with the loops that owned them; nothing runs on those now.
    sessionPools_.clear();
    if (controlFailure)
        std::rethrow_exception(controlFailure);
}

Server::Stats Server::stats() const
{
    Stats result;
    if (loopsReady_.load(std::memory_order_acquire)) {
        for (const auto& loop : loops_) {
            result.reactorTasksFailed += loop->failedTasks();
            result.reactorTasksRejected += loop->rejectedTasks();
            result.reactorQueueDepth += loop->pendingTasks();
        }
    }
    std::lock_guard<std::mutex> lock(poolMutex_);
    if (pool_) {
        result.workerTasksFailed = pool_->failedTasks();
        result.workerTasksRejected = pool_->rejectedTasks();
        result.workerQueueDepth = pool_->pendingTasks();
    }
    return result;
}

void Server::stop()
{
    if (!running_.exchange(false))
        return;

    stopCv_.notify_all();
}

}  // namespace http
