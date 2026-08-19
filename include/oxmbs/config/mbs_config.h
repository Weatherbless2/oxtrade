#pragma once

#include "oxmbs/logging/log_level.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace oxmbs::config {

struct MbsConfig {
    std::string service_name;  // 日志和服务识别使用的名称，例如 oxMBS。
    std::string environment;  // 当前运行环境，例如 development、test 或 production。
    std::string listen_host;  // 后续网络服务绑定的地址；本工单只负责保存配置。
    std::uint16_t listen_port{0};  // 网络端口，解析时限制在 1~65535。
    std::size_t query_worker_count{0};  // 查询类 Worker 的目标数量，解析时限制在 1~128。
    std::uint32_t shutdown_timeout_ms{0};  // 停止 Worker 的超时时间，单位为毫秒。
    std::filesystem::path log_directory;  // run 日志目录，不存在时由日志器创建。
    logging::LogLevel log_level{logging::LogLevel::Info};  // 日志最低输出级别，默认 INFO。
};

class MbsConfigLoader {
public:
    static bool load(const std::filesystem::path& path,
                     MbsConfig& config,
                     std::string& error);  // 成功写入完整配置，失败保留调用方原配置并返回诊断信息。
};

}  // namespace oxmbs::config
