#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace oxmbs::runtime {

struct SystemInfo {
    std::string cpu_model;  // CPU 型号文本；Linux 从 /proc/cpuinfo 读取。
    std::uint32_t logical_cpu_count{0};  // 逻辑 CPU 数量，用于启动摘要和资源监控。
    std::uint64_t memory_total_bytes{0};  // 主机总内存，统一换算为字节。
    std::uint64_t memory_available_bytes{0};  // 当前可用内存，统一换算为字节。
    std::uint64_t disk_total_bytes{0};  // storage_path 所在文件系统容量。
    std::uint64_t disk_available_bytes{0};  // storage_path 所在文件系统可用空间。
};

class SystemInfoCollector {
public:
    static bool collect(const std::filesystem::path& storage_path,
                        SystemInfo& info,
                        std::string& error);  // 一次性采集系统数据，失败返回可读错误。
};

}  // namespace oxmbs::runtime
