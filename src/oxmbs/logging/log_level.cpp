#include "oxmbs/logging/log_level.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace oxmbs::logging {
namespace {

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

}  // namespace

const char* log_level_name(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Debug:
        return "DEBUG";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    }
    return "UNKNOWN";
}

bool parse_log_level(std::string_view value, LogLevel& level) noexcept {
    const std::string normalized = upper(std::string(value));
    if (normalized == "DEBUG") {
        level = LogLevel::Debug;
    } else if (normalized == "INFO") {
        level = LogLevel::Info;
    } else if (normalized == "WARN" || normalized == "WARNING") {
        level = LogLevel::Warn;
    } else if (normalized == "ERROR") {
        level = LogLevel::Error;
    } else {
        return false;
    }
    return true;
}

}  // namespace oxmbs::logging
