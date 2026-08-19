#pragma once

#include <string_view>

namespace oxmbs::logging {

enum class LogLevel {
    Debug = 0,  // 最详细的调试信息。
    Info = 1,   // 正常生命周期和关键运行信息。
    Warn = 2,   // 可恢复或需要关注的异常情况。
    Error = 3,  // 当前操作失败或服务无法继续的错误。
};

const char* log_level_name(LogLevel level) noexcept;  // 将枚举转换为稳定的大写文本，用于日志格式化。
bool parse_log_level(std::string_view value, LogLevel& level) noexcept;  // 大小写不敏感地解析配置文本。

}  // namespace oxmbs::logging
