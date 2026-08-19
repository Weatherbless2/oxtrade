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
    std::filesystem::path directory;  // 日志文件所在目录。
    std::string service_name{"oxMBS"};  // 文件名前缀和每行日志中的 module 字段。
    LogLevel minimum_level{LogLevel::Info};  // 低于该级别的日志不会写出。
    bool mirror_stdout{true};  // 是否同时复制到标准输出，便于 docker logs 查看。
};

class RunLogger {
public:
    RunLogger() = default;
    ~RunLogger();  // 析构时刷新并关闭文件，避免日志句柄泄漏。

    RunLogger(const RunLogger&) = delete;
    RunLogger& operator=(const RunLogger&) = delete;

    bool open(const RunLoggerOptions& options, std::string& error);  // 创建目录和本次运行独立日志文件。
    void close() noexcept;  // 刷新并关闭文件；重复调用也是安全的。
    void log(LogLevel level, std::string_view message);  // 统一入口，负责加锁、过滤和写出。
    void debug(std::string_view message);  // Debug 级别便捷接口。
    void info(std::string_view message);   // Info 级别便捷接口。
    void warn(std::string_view message);   // Warn 级别便捷接口。
    void error(std::string_view message);  // Error 级别便捷接口。

    bool is_open() const noexcept;  // 查询当前文件是否已打开。
    const std::filesystem::path& log_path() const noexcept;  // 返回最近一次 open() 选择的文件路径。

private:
    void write_locked(LogLevel level, std::string_view message);  // 要求调用者已持有 mutex_。

    mutable std::mutex mutex_;  // 保护文件流、日志选项和整行输出的并发访问。
    std::ofstream stream_;  // 当前运行对应的日志文件流。
    std::filesystem::path log_path_;  // 当前运行的唯一日志路径。
    std::string service_name_;  // 写入日志行的 module 字段。
    LogLevel minimum_level_{LogLevel::Info};  // 运行时最低日志级别。
    bool mirror_stdout_{true};  // 是否将同一行复制到 stdout。
};

}  // namespace oxmbs::logging
