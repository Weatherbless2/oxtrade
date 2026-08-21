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
    // 配置文件允许等号两侧和 section/key 周围存在空白，先统一去除。
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
    // section 和 key 采用大小写不敏感匹配，规范化后再进入白名单判断。
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool parse_unsigned(std::string_view raw, std::uint64_t& value) {
    if (raw.empty() || raw.front() == '-') {  // 空值和负数都不属于无符号配置。
        return false;
    }

    std::uint64_t parsed = 0;
    for (const char ch : raw) {  // 手动解析，避免 stoi 异常和隐式截断。
        if (ch < '0' || ch > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {  // 乘十前检查溢出。
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    value = parsed;
    return true;
}

bool is_known_key(std::string_view section, std::string_view key) {
    // 只允许工单约定的字段，防止拼写错误悄悄变成无效配置。
    if (section == "service") {
        return key == "name" || key == "environment";
    }
    if (section == "server") {
        return key == "host" || key == "port";
    }
    if (section == "worker") {
        return key == "query_worker_count" || key == "shutdown_timeout_ms";
    }
    if (section == "lbm") {
        return key == "funclist_file";
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
    static constexpr std::string_view required[] = {  // 所有字段都必须显式出现，避免依赖默认值误启动。
        "service.name", "service.environment", "server.host", "server.port",
        "worker.query_worker_count", "worker.shutdown_timeout_ms", "logging.directory",
        "logging.level", "lbm.funclist_file",
    };
    for (const auto key : required) {
        if (!has_key(seen, key)) {
            error = "missing required key '" + std::string(key) + "'";
            return false;
        }
    }

    if (config.service_name.empty() || config.environment.empty() ||
        config.listen_host.empty() || config.funclist_path.empty() || config.log_directory.empty()) {
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
    std::ifstream stream(path);  // 在解析前打开文件；失败时不修改调用方传入的 config。
    if (!stream) {
        error = "cannot open config file '" + path.string() + "'";
        return false;
    }

    MbsConfig candidate;  // 先写入临时对象，完整校验通过后再一次性提交。
    std::unordered_set<std::string> seen;  // 记录完整键名，用于检测重复配置。
    std::string section;  // 当前 section，例如 service、server、worker 或 logging。
    std::string line;
    std::size_t line_number = 0;  // 所有语法错误都带行号，方便定位配置问题。
    while (std::getline(stream, line)) {  // 逐行消费 ini；每行只影响候选 config，不直接修改正在运行的配置。
        ++line_number;  // 保存原始行号，后续错误可以准确告诉维护者哪一行阻止启动。
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string value = trim(line);  // 去除行首尾空白后再判断内容。
        if (value.empty() || value.front() == '#' || value.front() == ';') {  // 空行和两种注释风格直接跳过。
            continue;
        }
        if (value.front() == '[') {  // section 行必须形如 [section]，后续键名解释依赖当前段。
            if (value.size() < 3 || value.back() != ']') {
                error = "line " + std::to_string(line_number) + ": invalid section";
                return false;
            }
            section = lower(trim(value.substr(1, value.size() - 2)));  // 去括号并统一小写，保证 [LBM] 与 [lbm] 行为一致。
            if (section != "service" && section != "server" && section != "worker" &&
                section != "lbm" && section != "logging") {
                error = "line " + std::to_string(line_number) + ": unknown section '" + section + "'";
                return false;
            }
            continue;
        }

        const auto separator = value.find('=');  // 第一个等号分开 key/value，value 内部的等号继续保留。
        if (section.empty() || separator == std::string::npos) {
            error = "line " + std::to_string(line_number) + ": expected section and key=value";
            return false;
        }
        const std::string key = lower(trim(value.substr(0, separator)));  // key 大小写不敏感。
        const std::string parsed_value = trim(value.substr(separator + 1));  // value 保留内部字符但去除外侧空白。
        if (!is_known_key(section, key)) {
            error = "line " + std::to_string(line_number) + ": unknown key '" + section + "." + key + "'";
            return false;
        }
        const std::string full_key = section + "." + key;  // 用完整键名区分不同 section 中的同名字段。
        if (!seen.insert(full_key).second) {
            error = "line " + std::to_string(line_number) + ": duplicate key '" + full_key + "'";
            return false;
        }

        if (section == "service" && key == "name") {  // 字符串字段直接写入候选配置，尚未成为运行时状态。
            candidate.service_name = parsed_value;  // 后续日志器会使用这个服务名。
        } else if (section == "service" && key == "environment") {
            candidate.environment = parsed_value;
        } else if (section == "server" && key == "host") {
            candidate.listen_host = parsed_value;
        } else if (section == "server" && key == "port") {
            std::uint64_t parsed = 0;  // 先用宽类型解析，再检查目标字段范围。
            if (!parse_unsigned(parsed_value, parsed) || parsed > 65535) {
                error = "line " + std::to_string(line_number) + ": server.port is invalid";
                return false;
            }
            candidate.listen_port = static_cast<std::uint16_t>(parsed);  // 范围确认后再安全缩窄。
        } else if (section == "worker" && key == "query_worker_count") {
            std::uint64_t parsed = 0;
            if (!parse_unsigned(parsed_value, parsed) || parsed > std::numeric_limits<std::size_t>::max()) {
                error = "line " + std::to_string(line_number) + ": worker.query_worker_count is invalid";
                return false;
            }
            candidate.query_worker_count = static_cast<std::size_t>(parsed);  // 范围确认后转换为平台 size 类型。
        } else if (section == "worker" && key == "shutdown_timeout_ms") {
            std::uint64_t parsed = 0;
            if (!parse_unsigned(parsed_value, parsed) || parsed > std::numeric_limits<std::uint32_t>::max()) {
                error = "line " + std::to_string(line_number) + ": worker.shutdown_timeout_ms is invalid";
                return false;
            }
            candidate.shutdown_timeout_ms = static_cast<std::uint32_t>(parsed);  // 范围确认后转换为毫秒字段。
        } else if (section == "lbm" && key == "funclist_file") {  // 工单 8 的入口字段决定后续加载哪张 LBM 路由表。
            const std::filesystem::path configured_path = parsed_value;  // 先保留用户配置的原始相对/绝对路径语义。
            candidate.funclist_path = configured_path.is_absolute()
                                          ? configured_path.lexically_normal()
                                          : (path.parent_path() / configured_path).lexically_normal();  // 配置引用以配置文件目录为基准，避免服务 cwd 改变后加载错误 XML。
        } else if (section == "logging" && key == "directory") {
            candidate.log_directory = parsed_value;
        } else if (section == "logging" && key == "level") {
            if (!logging::parse_log_level(parsed_value, candidate.log_level)) {
                error = "line " + std::to_string(line_number) + ": logging.level must be DEBUG, INFO, WARN or ERROR";
                return false;
            }
        }
    }

    if (!validate(candidate, seen, error)) {  // 语法解析成功不代表业务约束正确，还需统一校验必填字段和范围。
        return false;  // 失败时保留调用方旧 config，主程序会记录错误并停止启动。
    }

    config = std::move(candidate);  // 所有字段通过后一次性提交，避免半成品配置被 Logger/Worker 使用。
    return true;  // main.cpp 收到 true 后才会打开日志、解析 funclist 和创建队列。
}

}  // namespace oxmbs::config
