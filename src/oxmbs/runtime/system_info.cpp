#include "oxmbs/runtime/system_info.h"

#include <fstream>
#include <thread>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace oxmbs::runtime {
namespace {

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool read_linux_memory(std::uint64_t& total, std::uint64_t& available) {
    std::ifstream stream("/proc/meminfo");
    if (!stream) {
        return false;
    }

    std::string key;
    std::uint64_t value = 0;
    std::string unit;
    while (stream >> key >> value >> unit) {
        if (key == "MemTotal:") {
            total = value * 1024;
        } else if (key == "MemAvailable:") {
            available = value * 1024;
        }
    }
    return total > 0;
}

std::string read_linux_cpu_model() {
    std::ifstream stream("/proc/cpuinfo");
    std::string line;
    while (std::getline(stream, line)) {
        const auto delimiter = line.find(':');
        if (delimiter == std::string::npos) {
            continue;
        }
        const auto key = trim(line.substr(0, delimiter));
        if (key == "model name" || key == "Hardware") {
            return line.substr(delimiter + 1);
        }
    }
    return "unknown";
}

}  // namespace

bool SystemInfoCollector::collect(const std::filesystem::path& storage_path,
                                  SystemInfo& info,
                                  std::string& error) {
    if (storage_path.empty()) {
        error = "storage path must not be empty";
        return false;
    }

    std::error_code filesystem_error;
    const auto disk = std::filesystem::space(storage_path, filesystem_error);
    if (filesystem_error) {
        error = "cannot inspect storage path '" + storage_path.string() + "': " +
                filesystem_error.message();
        return false;
    }

    info = {};
    info.disk_total_bytes = disk.capacity;
    info.disk_available_bytes = disk.available;
    info.logical_cpu_count = std::thread::hardware_concurrency();
    if (info.logical_cpu_count == 0) {
        info.logical_cpu_count = 1;
    }

#if defined(__linux__)
    info.cpu_model = read_linux_cpu_model();
    if (!read_linux_memory(info.memory_total_bytes, info.memory_available_bytes)) {
        error = "cannot read Linux memory information from /proc/meminfo";
        return false;
    }
#elif defined(_WIN32)
    SYSTEM_INFO system_info{};
    ::GetSystemInfo(&system_info);
    info.logical_cpu_count = system_info.dwNumberOfProcessors;
    MEMORYSTATUSEX memory_status{};
    memory_status.dwLength = sizeof(memory_status);
    if (!::GlobalMemoryStatusEx(&memory_status)) {
        error = "cannot read Windows memory information";
        return false;
    }
    info.cpu_model = "Windows CPU";
    info.memory_total_bytes = memory_status.ullTotalPhys;
    info.memory_available_bytes = memory_status.ullAvailPhys;
#else
    info.cpu_model = "unknown";
    error = "system information is not implemented for this platform";
    return false;
#endif

    return true;
}

}  // namespace oxmbs::runtime
