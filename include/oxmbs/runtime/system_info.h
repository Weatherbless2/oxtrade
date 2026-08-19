#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace oxmbs::runtime {

struct SystemInfo {
    std::string cpu_model;
    std::uint32_t logical_cpu_count{0};
    std::uint64_t memory_total_bytes{0};
    std::uint64_t memory_available_bytes{0};
    std::uint64_t disk_total_bytes{0};
    std::uint64_t disk_available_bytes{0};
};

class SystemInfoCollector {
public:
    static bool collect(const std::filesystem::path& storage_path,
                        SystemInfo& info,
                        std::string& error);
};

}  // namespace oxmbs::runtime
