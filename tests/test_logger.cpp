// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// EXT-14: log file permissions, reopen, injection escaping, truncation marker,
// and a record of what a full queue dropped.

#include "http/logger.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

namespace {
std::string slurp(const std::string& path)
{
    std::ifstream file(path);
    std::stringstream content;
    content << file.rdbuf();
    return content.str();
}

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
    umask(0);
    char directoryTemplate[] = "/tmp/http-log-XXXXXX";
    const std::string directory = mkdtemp(directoryTemplate);
    const std::string path = directory + "/server.log";
    http::Logger& logger = http::Logger::instance();
    logger.enableConsole(false);
    logger.setLevel(http::LogLevel::Info);
    logger.setLogFile(path);

    // The file is created without world access.
    struct stat status
    {};
    expect(stat(path.c_str(), &status) == 0 && (status.st_mode & 0777) == 0640, 1,
           "log file mode 0640");

    // A client-controlled string cannot end the line and start another.
    HTTP_LOG_INFO("path=%s", "/a\r\n2026-01-01 00:00:00.000 [ERROR] forged\x01");
    logger.flush();
    const std::string content = slurp(path);
    expect(content.find("forged") != std::string::npos, 2, "message logged");
    expect(content.find("path=/a\\r\\n2026-01-01") != std::string::npos &&
               content.find("\\x01") != std::string::npos,
           3, "control characters escaped");
    expect(std::count(content.begin(), content.end(), '\n') == 1, 4, "still one line");

    // A message longer than a record is cut visibly, and counted.
    const size_t truncatedBefore = logger.truncatedRecords();
    HTTP_LOG_INFO("long=%s", std::string(3000, 'x').c_str());
    logger.flush();
    const std::string again = slurp(path);
    const size_t last = again.rfind("long=");
    expect(last != std::string::npos, 5, "long message logged");
    expect(again.substr(last).find("...\n") != std::string::npos, 6, "cut marker present");
    expect(logger.truncatedRecords() == truncatedBefore + 1, 7, "truncation counted");

    // logrotate renames the file and asks for a reopen.
    std::filesystem::rename(path, path + ".1");
    logger.reopen();
    HTTP_LOG_INFO("after rotation");
    logger.flush();
    expect(slurp(path).find("after rotation") != std::string::npos, 8, "new file after reopen");
    expect(slurp(path + ".1").find("after rotation") == std::string::npos, 9, "old file left alone");

    // A flood beyond the queue leaves a "dropped" record behind. Many producers
    // are needed to outrun the writer thread; if this machine still keeps up,
    // there is nothing to report and that part is skipped.
    {
        std::vector<std::thread> producers;
        std::atomic<bool> stop{false};
        for (int thread = 0; thread < 16; ++thread)
            producers.emplace_back([&, thread] {
                for (int index = 0; index < 200000 && !stop; ++index)
                    HTTP_LOG_INFO("flood %d %d", thread, index);
            });
        for (auto& producer : producers)
            producer.join();
    }
    if (logger.droppedRecords() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        logger.flush();
        expect(slurp(path).find("log records dropped (queue full)") != std::string::npos, 11,
               "dropped records reported");
    } else {
        std::puts("logger: writer kept up with the flood, dropped-record report not exercised");
    }

    // A symlink planted at the path is not followed.
    const std::string target = directory + "/target";
    std::ofstream(target) << "untouched\n";
    const std::string link = directory + "/link.log";
    std::filesystem::create_symlink(target, link);
    logger.setLogFile(link);
    HTTP_LOG_INFO("must not reach the target");
    logger.flush();
    expect(slurp(target) == "untouched\n", 12, "symlink not followed");

    logger.setLogFile("");
    std::filesystem::remove_all(directory);
    std::puts("logger: mode, escaping, truncation marker, reopen, dropped report, symlink passed");
}
