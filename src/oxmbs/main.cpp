#include "oxmbs/config/mbs_config.h"
#include "oxmbs/logging/run_logger.h"
#include "oxmbs/runtime/system_info.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

void print_usage(const char* program) {
    std::cout << "usage: " << program << " [config-path]\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        print_usage(argv[0]);
        return 2;
    }

    const std::filesystem::path config_path = argc == 2 ? argv[1] : "config/mbsconfig.ini";
    oxmbs::config::MbsConfig config;
    std::string error;
    if (!oxmbs::config::MbsConfigLoader::load(config_path, config, error)) {
        std::cerr << "oxMBS configuration error: " << error << '\n';
        return 1;
    }

    oxmbs::logging::RunLogger logger;
    oxmbs::logging::RunLoggerOptions options;
    options.directory = config.log_directory;
    options.service_name = config.service_name;
    options.minimum_level = config.log_level;
    if (!logger.open(options, error)) {
        std::cerr << "oxMBS logger error: " << error << '\n';
        return 1;
    }

    oxmbs::runtime::SystemInfo system_info;
    if (!oxmbs::runtime::SystemInfoCollector::collect(config.log_directory, system_info, error)) {
        logger.error(error);
        return 1;
    }

    logger.info("service started; environment=" + config.environment);
    logger.info("listen=" + config.listen_host + ":" + std::to_string(config.listen_port));
    logger.info("query_worker_count=" + std::to_string(config.query_worker_count));
    logger.info("cpu=" + system_info.cpu_model + ", logical_cpu_count=" +
                std::to_string(system_info.logical_cpu_count));
    logger.info("memory_total_bytes=" + std::to_string(system_info.memory_total_bytes) +
                ", memory_available_bytes=" + std::to_string(system_info.memory_available_bytes));
    logger.info("disk_total_bytes=" + std::to_string(system_info.disk_total_bytes) +
                ", disk_available_bytes=" + std::to_string(system_info.disk_available_bytes));
    logger.info("service stopped");
    return 0;
}
