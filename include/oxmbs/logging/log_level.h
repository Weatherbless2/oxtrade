#pragma once

#include <string_view>

namespace oxmbs::logging {

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
};

const char* log_level_name(LogLevel level) noexcept;
bool parse_log_level(std::string_view value, LogLevel& level) noexcept;

}  // namespace oxmbs::logging
