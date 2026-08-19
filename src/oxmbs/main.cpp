#include "oxmbs/config/mbs_config.h"
#include "oxmbs/logging/run_logger.h"
#include "oxmbs/runtime/system_info.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

void print_usage(const char* program) {
    std::cout << "usage: " << program << " [config-path]\n";  // 命令行只允许一个可选配置路径。
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2) {  // 多余参数视为调用错误，不进入配置解析。
        print_usage(argv[0]);
        return 2;
    }

    const std::filesystem::path config_path = argc == 2 ? argv[1] : "config/mbsconfig.ini";  // 未传参使用默认路径。
    oxmbs::config::MbsConfig config;
    std::string error;
    if (!oxmbs::config::MbsConfigLoader::load(config_path, config, error)) {  // 配置失败时先输出错误，不创建日志器。
        std::cerr << "oxMBS configuration error: " << error << '\n';
        return 1;
    }

    oxmbs::logging::RunLogger logger;
    oxmbs::logging::RunLoggerOptions options;  // 将配置对象转换为日志器所需选项。
    options.directory = config.log_directory;
    options.service_name = config.service_name;
    options.minimum_level = config.log_level;
    if (!logger.open(options, error)) {  // 目录或文件不可写时终止启动。
        std::cerr << "oxMBS logger error: " << error << '\n';
        return 1;
    }

    oxmbs::runtime::SystemInfo system_info;
    if (!oxmbs::runtime::SystemInfoCollector::collect(config.log_directory, system_info, error)) {  // 系统信息失败记录到 run 日志。
        logger.error(error);
        return 1;
    }

    logger.info("service started; environment=" + config.environment);  // 启动摘要第一行。
    logger.info("listen=" + config.listen_host + ":" + std::to_string(config.listen_port));
    logger.info("query_worker_count=" + std::to_string(config.query_worker_count));
    logger.info("cpu=" + system_info.cpu_model + ", logical_cpu_count=" +
                std::to_string(system_info.logical_cpu_count));
    logger.info("memory_total_bytes=" + std::to_string(system_info.memory_total_bytes) +
                ", memory_available_bytes=" + std::to_string(system_info.memory_available_bytes));
    logger.info("disk_total_bytes=" + std::to_string(system_info.disk_total_bytes) +
                ", disk_available_bytes=" + std::to_string(system_info.disk_available_bytes));
    logger.info("service stopped");  // 当前 bootstrap 没有长驻业务循环，记录后正常退出。
    return 0;
}
