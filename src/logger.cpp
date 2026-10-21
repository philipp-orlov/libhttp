// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "http/logger.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <cstring>
#include <string>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace http {

Logger& Logger::instance()
{
    static Logger instance;
    return instance;
}

Logger::Logger()
{
    pending_.reserve(1024);
    draining_.reserve(1024);
    worker_ = std::thread(&Logger::workerLoop, this);
}

Logger::~Logger()
{
    flush();
    running_.store(false, std::memory_order_relaxed);
    queue_cv_.notify_all();
    if (worker_.joinable())
        worker_.join();

    if (file_)
        std::fclose(file_);
}

void Logger::setLogFile(const std::string& path, unsigned mode)
{
    std::lock_guard<std::mutex> lock(file_mutex_);
    file_path_ = path;
    file_mode_ = mode;
    openFile();
}

void Logger::reopen()
{
    std::lock_guard<std::mutex> lock(file_mutex_);
    openFile();
}

// Caller holds file_mutex_.
bool Logger::openFile()
{
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
    if (file_path_.empty())
        return true;

#if defined(_WIN32)
    file_ = std::fopen(file_path_.c_str(), "a");
#else
    // Created with the requested mode; a symlink at the path is not followed.
    const int descriptor = ::open(file_path_.c_str(),
                                  O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW,
                                  static_cast<mode_t>(file_mode_));
    if (descriptor >= 0) {
        file_ = ::fdopen(descriptor, "a");
        if (!file_)
            ::close(descriptor);
    }
#endif
    return file_ != nullptr;
}

const char* Logger::levelName(LogLevel level)
{
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO";
        case LogLevel::Warn: return "WARN";
        case LogLevel::Error: return "ERROR";
        default: return "OFF";
    }
}

Logger::Record Logger::compose(LogLevel level, std::string_view message)
{
    using namespace std::chrono;
    auto now = system_clock::now();
    auto t = system_clock::to_time_t(now);
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tmBuf{};
#if defined(_WIN32)
    localtime_s(&tmBuf, &t);
#else
    localtime_r(&t, &tmBuf);
#endif

    char line[512];
    int prefixLen =
        std::snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] ",
                      tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday, tmBuf.tm_hour,
                      tmBuf.tm_min, tmBuf.tm_sec, static_cast<int>(ms.count()), levelName(level));
    if (prefixLen < 0)
        prefixLen = 0;

    Record entry{};
    const size_t prefixBytes = std::min(static_cast<size_t>(prefixLen), sizeof(line) - 1);
    std::memcpy(entry.text.data(), line, prefixBytes);

    // Room for the message, the marker of a cut one and the newline.
    const size_t limit = entry.text.size() - 1;
    size_t length = prefixBytes;
    bool cut = false;
    static const char kHex[] = "0123456789abcdef";
    for (const char c : message) {
        const unsigned char v = static_cast<unsigned char>(c);
        char escaped[4];
        size_t width = 1;
        escaped[0] = c;
        if (v == '\n' || v == '\r' || v == '\t') {
            escaped[0] = '\\';
            escaped[1] = v == '\n' ? 'n' : (v == '\r' ? 'r' : 't');
            width = 2;
        } else if (v < 0x20 || v == 0x7f) {
            escaped[0] = '\\';
            escaped[1] = 'x';
            escaped[2] = kHex[v >> 4];
            escaped[3] = kHex[v & 15];
            width = 4;
        }
        if (length + width + 3 > limit) {
            cut = true;
            break;
        }
        std::memcpy(entry.text.data() + length, escaped, width);
        length += width;
    }
    if (cut) {
        std::memcpy(entry.text.data() + length, "...", 3);
        length += 3;
        ++truncated_;
    }
    entry.length = length;
    entry.text[entry.length++] = '\n';
    return entry;
}

void Logger::log(LogLevel level, std::string_view message)
{
    if (level < level_.load(std::memory_order_relaxed))
        return;

    Record entry = compose(level, message);

    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (pending_.size() == pending_.capacity()) {
        ++dropped_;
        return;
    }
    pending_.push_back(std::move(entry));
    queue_cv_.notify_one();
}

void Logger::workerLoop()
{
    while (running_.load(std::memory_order_relaxed)) {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(200), [this] {
                return !pending_.empty() || flush_requested_.load(std::memory_order_relaxed) ||
                       !running_.load(std::memory_order_relaxed);
            });
            std::swap(pending_, draining_);
        }

        // Records lost to a full queue leave a trace of their own, at most once a second.
        const size_t dropped = dropped_.load(std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        if (dropped != reported_dropped_ && now - last_drop_report_ >= std::chrono::seconds(1)) {
            const std::string text = std::to_string(dropped - reported_dropped_) +
                                     " log records dropped (queue full)";
            reported_dropped_ = dropped;
            last_drop_report_ = now;
            draining_.push_back(compose(LogLevel::Warn, text));
        }

        if (!draining_.empty()) {
            if (console_enabled_.load(std::memory_order_relaxed)) {
                for (auto& line : draining_)
                    std::fwrite(line.data(), 1, line.size(), stdout);

                std::fflush(stdout);
            }
            {
                std::lock_guard<std::mutex> lock(file_mutex_);
                if (file_) {
                    for (auto& line : draining_)
                        std::fwrite(line.data(), 1, line.size(), file_);

                    std::fflush(file_);
                }
            }
            draining_.clear();
        }

        if (flush_requested_.load(std::memory_order_relaxed)) {
            flush_requested_.store(false, std::memory_order_relaxed);
            flush_cv_.notify_all();
        }
    }
}

void Logger::flush()
{
    std::unique_lock<std::mutex> lock(queue_mutex_);
    flush_requested_.store(true, std::memory_order_relaxed);
    queue_cv_.notify_all();
    flush_cv_.wait_for(lock, std::chrono::seconds(2),
                       [this] { return !flush_requested_.load(std::memory_order_relaxed); });
}

void logFormatted(LogLevel level, const char* fmt, ...)
{
    if (level < Logger::instance().level())
        return;

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (n < 0)
        return;

    if (static_cast<size_t>(n) >= sizeof(buf)) {
        // Longer than the stack buffer: format it whole (the logger cuts it to a record).
        std::string longer(static_cast<size_t>(n) + 1, '\0');
        va_start(args, fmt);
        std::vsnprintf(longer.data(), longer.size(), fmt, args);
        va_end(args);
        Logger::instance().log(level, std::string_view(longer.data(), static_cast<size_t>(n)));
        return;
    }

    Logger::instance().log(level, std::string_view(buf, static_cast<size_t>(n)));
}

}  // namespace http
