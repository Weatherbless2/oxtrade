#pragma once

#include "oxmbs/logging/log_level.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace oxmbs::logging {

struct RunLoggerOptions {
    std::filesystem::path directory;
    std::string service_name{"oxMBS"};
    LogLevel minimum_level{LogLevel::Info};
    bool mirror_stdout{true};
};

class RunLogger {
public:
    RunLogger() = default;
    ~RunLogger();

    RunLogger(const RunLogger&) = delete;
    RunLogger& operator=(const RunLogger&) = delete;

    bool open(const RunLoggerOptions& options, std::string& error);
    void close() noexcept;
    void log(LogLevel level, std::string_view message);
    void debug(std::string_view message);
    void info(std::string_view message);
    void warn(std::string_view message);
    void error(std::string_view message);

    bool is_open() const noexcept;
    const std::filesystem::path& log_path() const noexcept;

private:
    void write_locked(LogLevel level, std::string_view message);

    mutable std::mutex mutex_;
    std::ofstream stream_;
    std::filesystem::path log_path_;
    std::string service_name_;
    LogLevel minimum_level_{LogLevel::Info};
    bool mirror_stdout_{true};
};

}  // namespace oxmbs::logging
