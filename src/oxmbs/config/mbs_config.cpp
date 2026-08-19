#include "oxmbs/config/mbs_config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <string_view>
#include <unordered_set>

namespace oxmbs::config {
namespace {

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (first >= last) {
        return {};
    }
    return {first, last};
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool parse_unsigned(std::string_view raw, std::uint64_t& value) {
    if (raw.empty() || raw.front() == '-') {
        return false;
    }

    std::uint64_t parsed = 0;
    for (const char ch : raw) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    value = parsed;
    return true;
}

bool is_known_key(std::string_view section, std::string_view key) {
    if (section == "service") {
        return key == "name" || key == "environment";
    }
    if (section == "server") {
        return key == "host" || key == "port";
    }
    if (section == "worker") {
        return key == "query_worker_count" || key == "shutdown_timeout_ms";
    }
    if (section == "logging") {
        return key == "directory" || key == "level";
    }
    return false;
}

bool has_key(const std::unordered_set<std::string>& keys, std::string_view full_key) {
    return keys.find(std::string(full_key)) != keys.end();
}

bool validate(const MbsConfig& config,
              const std::unordered_set<std::string>& seen,
              std::string& error) {
    static constexpr std::string_view required[] = {
        "service.name", "service.environment", "server.host", "server.port",
        "worker.query_worker_count", "worker.shutdown_timeout_ms", "logging.directory",
        "logging.level",
    };
    for (const auto key : required) {
        if (!has_key(seen, key)) {
            error = "missing required key '" + std::string(key) + "'";
            return false;
        }
    }

    if (config.service_name.empty() || config.environment.empty() ||
        config.listen_host.empty() || config.log_directory.empty()) {
        error = "required string value is empty";
        return false;
    }
    if (config.listen_port == 0) {
        error = "server.port must be between 1 and 65535";
        return false;
    }
    if (config.query_worker_count == 0 || config.query_worker_count > 128) {
        error = "worker.query_worker_count must be between 1 and 128";
        return false;
    }
    if (config.shutdown_timeout_ms == 0 || config.shutdown_timeout_ms > 600000) {
        error = "worker.shutdown_timeout_ms must be between 1 and 600000";
        return false;
    }
    return true;
}

}  // namespace

bool MbsConfigLoader::load(const std::filesystem::path& path,
                           MbsConfig& config,
                           std::string& error) {
    std::ifstream stream(path);
    if (!stream) {
        error = "cannot open config file '" + path.string() + "'";
        return false;
    }

    MbsConfig candidate;
    std::unordered_set<std::string> seen;
    std::string section;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(stream, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string value = trim(line);
        if (value.empty() || value.front() == '#' || value.front() == ';') {
            continue;
        }
        if (value.front() == '[') {
            if (value.size() < 3 || value.back() != ']') {
                error = "line " + std::to_string(line_number) + ": invalid section";
                return false;
            }
            section = lower(trim(value.substr(1, value.size() - 2)));
            if (section != "service" && section != "server" && section != "worker" &&
                section != "logging") {
                error = "line " + std::to_string(line_number) + ": unknown section '" + section + "'";
                return false;
            }
            continue;
        }

        const auto separator = value.find('=');
        if (section.empty() || separator == std::string::npos) {
            error = "line " + std::to_string(line_number) + ": expected section and key=value";
            return false;
        }
        const std::string key = lower(trim(value.substr(0, separator)));
        const std::string parsed_value = trim(value.substr(separator + 1));
        if (!is_known_key(section, key)) {
            error = "line " + std::to_string(line_number) + ": unknown key '" + section + "." + key + "'";
            return false;
        }
        const std::string full_key = section + "." + key;
        if (!seen.insert(full_key).second) {
            error = "line " + std::to_string(line_number) + ": duplicate key '" + full_key + "'";
            return false;
        }

        if (section == "service" && key == "name") {
            candidate.service_name = parsed_value;
        } else if (section == "service" && key == "environment") {
            candidate.environment = parsed_value;
        } else if (section == "server" && key == "host") {
            candidate.listen_host = parsed_value;
        } else if (section == "server" && key == "port") {
            std::uint64_t parsed = 0;
            if (!parse_unsigned(parsed_value, parsed) || parsed > 65535) {
                error = "line " + std::to_string(line_number) + ": server.port is invalid";
                return false;
            }
            candidate.listen_port = static_cast<std::uint16_t>(parsed);
        } else if (section == "worker" && key == "query_worker_count") {
            std::uint64_t parsed = 0;
            if (!parse_unsigned(parsed_value, parsed) || parsed > std::numeric_limits<std::size_t>::max()) {
                error = "line " + std::to_string(line_number) + ": worker.query_worker_count is invalid";
                return false;
            }
            candidate.query_worker_count = static_cast<std::size_t>(parsed);
        } else if (section == "worker" && key == "shutdown_timeout_ms") {
            std::uint64_t parsed = 0;
            if (!parse_unsigned(parsed_value, parsed) || parsed > std::numeric_limits<std::uint32_t>::max()) {
                error = "line " + std::to_string(line_number) + ": worker.shutdown_timeout_ms is invalid";
                return false;
            }
            candidate.shutdown_timeout_ms = static_cast<std::uint32_t>(parsed);
        } else if (section == "logging" && key == "directory") {
            candidate.log_directory = parsed_value;
        } else if (section == "logging" && key == "level") {
            if (!logging::parse_log_level(parsed_value, candidate.log_level)) {
                error = "line " + std::to_string(line_number) + ": logging.level must be DEBUG, INFO, WARN or ERROR";
                return false;
            }
        }
    }

    if (!validate(candidate, seen, error)) {
        return false;
    }

    config = std::move(candidate);
    return true;
}

}  // namespace oxmbs::config
