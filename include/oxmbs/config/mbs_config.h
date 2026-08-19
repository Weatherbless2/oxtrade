#pragma once

#include "oxmbs/logging/log_level.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace oxmbs::config {

struct MbsConfig {
    std::string service_name;
    std::string environment;
    std::string listen_host;
    std::uint16_t listen_port{0};
    std::size_t query_worker_count{0};
    std::uint32_t shutdown_timeout_ms{0};
    std::filesystem::path log_directory;
    logging::LogLevel log_level{logging::LogLevel::Info};
};

class MbsConfigLoader {
public:
    static bool load(const std::filesystem::path& path,
                     MbsConfig& config,
                     std::string& error);
};

}  // namespace oxmbs::config
