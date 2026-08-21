#include "oxmbs/config/mbs_config.h"
#include "oxmbs/logging/run_logger.h"
#include "oxmbs/runtime/system_info.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::filesystem::path temporary_directory(const std::string& name) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();  // 避免测试之间目录冲突。
    const auto path = std::filesystem::temp_directory_path() /
                      ("oxmbs_" + name + "_" + std::to_string(suffix));
    std::filesystem::create_directories(path);
    return path;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream stream(path);  // 测试通过临时文件构造每种配置输入。
    assert(stream);
    stream << content;
    assert(stream.good());
}

std::string valid_config() {
    return "[service]\n"  // 基准合法配置，其他测试在此基础上构造错误。
           "name=oxMBS\n"
           "environment=test\n"
           "\n"
           "[server]\n"
           "host=0.0.0.0\n"
           "port=19001\n"
           "\n"
           "[worker]\n"
           "query_worker_count=4\n"
           "shutdown_timeout_ms=2500\n"
           "\n"
           "[lbm]\n"
           "funclist_file=funclist.xml\n"
           "\n"
           "[logging]\n"
           "directory=runtime-logs\n"
           "level=debug\n";
}

void test_valid_config() {
    const auto directory = temporary_directory("config_valid");
    const auto path = directory / "mbsconfig.ini";
    write_file(path, valid_config());

    oxmbs::config::MbsConfig config;
    std::string error;
    assert(oxmbs::config::MbsConfigLoader::load(path, config, error));  // 合法文本应完整转换为运行时对象。
    assert(config.service_name == "oxMBS");
    assert(config.environment == "test");
    assert(config.listen_host == "0.0.0.0");
    assert(config.listen_port == 19001);
    assert(config.query_worker_count == 4);
    assert(config.shutdown_timeout_ms == 2500);
    assert(config.funclist_path == directory / "funclist.xml");  // 相对 LBM 配置必须稳定锚定到 ini 文件目录。
    assert(config.log_directory == "runtime-logs");
    assert(config.log_level == oxmbs::logging::LogLevel::Debug);
    std::filesystem::remove_all(directory);
}

void test_invalid_config_is_diagnosable() {
    const auto directory = temporary_directory("config_invalid");
    const auto path = directory / "mbsconfig.ini";
    const auto base = valid_config();

    auto invalid_port = base;
    const auto port_position = invalid_port.find("port=19001");
    invalid_port.replace(port_position, std::string("port=19001").size(), "port=0");
    write_file(path, invalid_port);

    oxmbs::config::MbsConfig config;
    std::string error;
    assert(!oxmbs::config::MbsConfigLoader::load(path, config, error));  // 端口越界必须拒绝。
    assert(error.find("server.port") != std::string::npos);

    write_file(path, base + "unknown=value\n");
    assert(!oxmbs::config::MbsConfigLoader::load(path, config, error));  // 未知键必须拒绝并给出字段名。
    assert(error.find("unknown key") != std::string::npos);

    write_file(path, base + "[worker]\nquery_worker_count=2\n");
    assert(!oxmbs::config::MbsConfigLoader::load(path, config, error));  // 重复完整键必须拒绝。
    assert(error.find("duplicate key") != std::string::npos);
    std::filesystem::remove_all(directory);
}

void test_missing_config_is_diagnosable() {
    oxmbs::config::MbsConfig config;
    std::string error;
    assert(!oxmbs::config::MbsConfigLoader::load("/tmp/oxmbs-config-does-not-exist.ini", config, error));  // 文件不存在时返回可读错误。
    assert(error.find("cannot open config file") != std::string::npos);
}

void test_run_logger_creates_separate_file_and_filters_level() {
    const auto directory = temporary_directory("logger");
    oxmbs::logging::RunLogger logger;
    oxmbs::logging::RunLoggerOptions options;
    options.directory = directory / "nested";
    options.service_name = "oxMBS";
    options.minimum_level = oxmbs::logging::LogLevel::Info;
    options.mirror_stdout = false;
    std::string error;
    assert(logger.open(options, error));  // open() 应自动创建嵌套目录并生成独立日志文件。
    logger.debug("hidden debug");
    logger.info("startup info");
    logger.warn("warning detail");
    assert(logger.is_open());
    const auto log_path = logger.log_path();
    logger.close();  // 关闭后再读取文件，确保 flush 已完成。

    std::ifstream stream(log_path);
    assert(stream);
    std::stringstream content;
    content << stream.rdbuf();
    const auto text = content.str();
    assert(text.find("hidden debug") == std::string::npos);
    assert(text.find("[oxMBS][INFO]: startup info") != std::string::npos);
    assert(text.find("[oxMBS][WARN]: warning detail") != std::string::npos);
    assert(text.find("|[") != std::string::npos);
    std::filesystem::remove_all(directory);
}

void test_system_info() {
    const auto directory = temporary_directory("system_info");
    oxmbs::runtime::SystemInfo info;
    std::string error;
    assert(oxmbs::runtime::SystemInfoCollector::collect(directory, info, error));  // WSL/Linux 应能读取 CPU、内存和磁盘。
    assert(!info.cpu_model.empty());
    assert(info.logical_cpu_count > 0);
    assert(info.memory_total_bytes > 0);
    assert(info.disk_total_bytes > 0);
    assert(info.disk_available_bytes > 0);
    std::filesystem::remove_all(directory);
}

}  // namespace

int main() {
    test_valid_config();  // 配置正常路径。
    test_invalid_config_is_diagnosable();  // 配置语法/白名单/范围错误。
    test_missing_config_is_diagnosable();  // 文件打开错误。
    test_run_logger_creates_separate_file_and_filters_level();  // 日志文件、级别过滤和格式。
    test_system_info();  // 系统资源采集。
    return 0;
}
