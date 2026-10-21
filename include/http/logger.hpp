// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace http {

enum class LogLevel
{
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Off
};

// Async logger: the calling (worker/reactor) thread only formats a line and
// pushes it into a double-buffered queue -- no I/O and no syscall happens
// on that thread. A single background thread swaps buffers and writes to
// the configured sinks (console/file). This keeps logging cheap enough to
// leave on in the hot request path of a server doing thousands of request/s.
class Logger
{
public:
    static Logger& instance();

    void setLevel(LogLevel level) { level_.store(level, std::memory_order_relaxed); }
    LogLevel level() const { return level_.load(std::memory_order_relaxed); }

    void enableConsole(bool enabled) { console_enabled_.store(enabled); }

    // Opens (or replaces) the file sink. Pass empty string to disable. A
    // file it creates gets `mode` (before the umask): log lines carry client
    // addresses and request paths, so the default is not world-readable.
    void setLogFile(const std::string& path, unsigned mode = 0640);
    // Closes and opens the file sink's path again, e.g. after logrotate
    // renamed the file (call it from a SIGHUP handler's thread, not the handler).
    void reopen();

    // Control characters (CR, LF, ...) in the message are written as \r, \n,
    // \xNN so that data from a client cannot forge a log line. A message
    // longer than a record is cut and ends in "...".
    void log(LogLevel level, std::string_view message);

    // Flushes pending buffered lines and blocks until written. Call before
    // process exit to avoid losing the last batch of log lines.
    void flush();
    size_t droppedRecords() const { return dropped_.load(); }
    size_t truncatedRecords() const { return truncated_.load(); }

    ~Logger();

private:
    Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void workerLoop();
    static const char* levelName(LogLevel level);
    struct Record;
    Record compose(LogLevel level, std::string_view message);
    bool openFile();

    std::atomic<LogLevel> level_{LogLevel::Info};
    std::atomic<bool> console_enabled_{true};

    std::mutex file_mutex_;
    std::FILE* file_ = nullptr;
    std::string file_path_;
    unsigned file_mode_ = 0640;
    size_t reported_dropped_ = 0;  // worker thread only
    std::chrono::steady_clock::time_point last_drop_report_{};

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    struct Record
    {
        std::array<char, 1024> text;
        size_t length = 0;
        const char* data() const { return text.data(); }
        size_t size() const { return length; }
    };
    std::vector<Record> pending_;
    std::vector<Record> draining_;
    std::atomic<size_t> dropped_{0};
    std::atomic<size_t> truncated_{0};
    std::atomic<bool> running_{true};
    std::atomic<bool> flush_requested_{false};
    std::condition_variable flush_cv_;
    std::thread worker_;
};

// Formats "[time][level] message" and enqueues it. Cheap enough to call
// unconditionally; the level check short-circuits when logging is off.
void logFormatted(LogLevel level, const char* fmt, ...);

}  // namespace http

#define HTTP_LOG_TRACE(...) ::http::logFormatted(::http::LogLevel::Trace, __VA_ARGS__)
#define HTTP_LOG_DEBUG(...) ::http::logFormatted(::http::LogLevel::Debug, __VA_ARGS__)
#define HTTP_LOG_INFO(...) ::http::logFormatted(::http::LogLevel::Info, __VA_ARGS__)
#define HTTP_LOG_WARN(...) ::http::logFormatted(::http::LogLevel::Warn, __VA_ARGS__)
#define HTTP_LOG_ERROR(...) ::http::logFormatted(::http::LogLevel::Error, __VA_ARGS__)
