// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// http::Logger is a facade over lg::Logger (develop-core liblog). The level enums have the same order, so levels
// convert by value. The facade owns one lg::Logger with 1 KiB lines (libhttp's record size) and local timestamps.

#include "http/logger.hpp"

#include <mutex>

#include "lg/logger.hpp"
#include "lg/sink.hpp"

namespace http {

static_assert(static_cast<int>(LogLevel::Trace) == static_cast<int>(lg::Level::Trace) &&
                  static_cast<int>(LogLevel::Error) == static_cast<int>(lg::Level::Error) &&
                  static_cast<int>(LogLevel::Off) == static_cast<int>(lg::Level::Off),
              "http::LogLevel and lg::Level must stay in step");

namespace {
lg::Level toLg(LogLevel level) { return static_cast<lg::Level>(static_cast<int>(level)); }

lg::LoggerOptions facadeOptions()
{
    lg::LoggerOptions o;
    o.slotBytes = 1024;
    o.ringSlots = 256;
    o.level = lg::Level::Info;
    o.utc = false;
    return o;
}
}  // namespace

struct Logger::Impl
{
    lg::Logger log{facadeOptions()};
    std::mutex config_mutex;   // console / file settings and sink rebuilds
    bool console = true;
    std::string file_path;
    unsigned file_mode = 0640;
};

Logger& Logger::instance()
{
    static Logger instance;
    return instance;
}

Logger::Logger() : impl_(new Impl)
{
    std::lock_guard<std::mutex> lock(impl_->config_mutex);
    rebuildSinks();
}

Logger::~Logger()
{
    impl_->log.shutdown();
}

void Logger::setLevel(LogLevel level) { impl_->log.setLevel(toLg(level)); }
LogLevel Logger::level() const { return static_cast<LogLevel>(static_cast<int>(impl_->log.level())); }
bool Logger::enabled(LogLevel level) const { return level != LogLevel::Off && impl_->log.enabled(toLg(level)); }

// Lines already queued are written to the old sinks first, so a sink change never reorders or re-routes them.
void Logger::rebuildSinks()
{
    impl_->log.flush();
    impl_->log.clearSinks();
    if (impl_->console)
        impl_->log.addSink(lg::makeConsoleSink(1));
    if (!impl_->file_path.empty()) {
        lg::FileSinkOptions fo;
        fo.path = impl_->file_path;
        fo.mode = impl_->file_mode;
        if (auto sink = lg::makeFileSink(fo))
            impl_->log.addSink(std::move(sink));
    }
}

void Logger::enableConsole(bool enabled)
{
    std::lock_guard<std::mutex> lock(impl_->config_mutex);
    if (impl_->console == enabled)
        return;
    impl_->console = enabled;
    rebuildSinks();
}

void Logger::setLogFile(const std::string& path, unsigned mode)
{
    std::lock_guard<std::mutex> lock(impl_->config_mutex);
    impl_->file_path = path;
    impl_->file_mode = mode;
    rebuildSinks();
}

void Logger::reopen()
{
    std::lock_guard<std::mutex> lock(impl_->config_mutex);
    impl_->log.flush();
    impl_->log.reopenSinks();
}

void Logger::log(LogLevel level, std::string_view message)
{
    if (enabled(level))
        impl_->log.writeRaw(toLg(level), message);
}

void Logger::logV(LogLevel level, const char* fmt, va_list args)
{
    if (enabled(level))
        impl_->log.writeV(toLg(level), fmt, args);
}

void Logger::flush() { impl_->log.flush(); }
size_t Logger::droppedRecords() const { return static_cast<size_t>(impl_->log.droppedLines()); }
size_t Logger::truncatedRecords() const { return static_cast<size_t>(impl_->log.truncatedLines()); }

void logFormatted(LogLevel level, const char* fmt, ...)
{
    Logger& logger = Logger::instance();
    if (!logger.enabled(level))
        return;
    va_list args;
    va_start(args, fmt);
    logger.logV(level, fmt, args);
    va_end(args);
}

}  // namespace http
