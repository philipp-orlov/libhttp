// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdarg>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
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

// Async logger, now a thin facade over libcore's liblog (lg::Logger): each producer thread formats straight
// into its own lock-free ring (no lock, no syscall, no allocation after its first line); one consumer thread
// escapes control characters, adds the local-time prefix and writes to the sinks (console on stdout, file).
class Logger
{
public:
    static Logger& instance();

    void setLevel(LogLevel level);
    LogLevel level() const;
    bool enabled(LogLevel level) const;

    void enableConsole(bool enabled);

    // Opens (or replaces) the file sink. Pass empty string to disable. A
    // file it creates gets `mode` (before the umask): log lines carry client
    // addresses and request paths, so the default is not world-readable.
    // A symlink at the path is not followed (the file sink is not opened).
    void setLogFile(const std::string& path, unsigned mode = 0640);
    // Closes and opens the file sink's path again, e.g. after logrotate
    // renamed the file (call it from a SIGHUP handler's thread, not the handler).
    void reopen();

    // Control characters (CR, LF, ...) in the message are written as \r, \n,
    // \xNN so that data from a client cannot forge a log line. A message
    // longer than a record (1 KiB) is cut and ends in "...".
    void log(LogLevel level, std::string_view message);
    void logV(LogLevel level, const char* fmt, va_list args);

    // Blocks until every line logged before the call has been written.
    void flush();
    size_t droppedRecords() const;
    size_t truncatedRecords() const;

    ~Logger();

private:
    Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    void rebuildSinks();   // caller holds config_mutex_

    struct Impl;
    std::unique_ptr<Impl> impl_;
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
