#include "oxmbs/logging/run_logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <Windows.h>
#endif

namespace oxmbs::logging {
namespace {

std::string timestamp(std::chrono::system_clock::time_point now, bool with_milliseconds) {
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now.time_since_epoch()) % 1000;
    std::tm local_time{};
#if defined(_WIN32)
    localtime_s(&local_time, &time);
#else
    localtime_r(&time, &local_time);
#endif
    std::ostringstream output;
    output << std::put_time(&local_time, with_milliseconds ? "%Y%m%d %H%M%S" : "%Y%m%d_%H%M%S");
    if (with_milliseconds) {
        output << std::setfill('0') << std::setw(3) << milliseconds.count();
    }
    return output.str();
}

std::uint64_t process_id() noexcept {
#if defined(__linux__)
    return static_cast<std::uint64_t>(::getpid());
#elif defined(_WIN32)
    return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
    return 0;
#endif
}

std::uint64_t thread_id() noexcept {
#if defined(__linux__)
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#elif defined(_WIN32)
    return static_cast<std::uint64_t>(::GetCurrentThreadId());
#else
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
#endif
}

std::filesystem::path unique_log_path(const std::filesystem::path& directory,
                                      const std::string& service_name) {
    const auto base = service_name + "_" + timestamp(std::chrono::system_clock::now(), false);
    auto candidate = directory / (base + ".log");
    for (unsigned int suffix = 1; std::filesystem::exists(candidate); ++suffix) {
        candidate = directory / (base + "_" + std::to_string(suffix) + ".log");
    }
    return candidate;
}

}  // namespace

RunLogger::~RunLogger() {
    close();
}

bool RunLogger::open(const RunLoggerOptions& options, std::string& error) {
    std::lock_guard lock(mutex_);
    if (options.directory.empty() || options.service_name.empty()) {
        error = "logger directory and service name must not be empty";
        return false;
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(options.directory, filesystem_error);
    if (filesystem_error) {
        error = "cannot create log directory '" + options.directory.string() + "': " +
                filesystem_error.message();
        return false;
    }

    const auto candidate = unique_log_path(options.directory, options.service_name);
    stream_.open(candidate, std::ios::out | std::ios::app);
    if (!stream_) {
        error = "cannot open run log '" + candidate.string() + "'";
        return false;
    }
    log_path_ = candidate;
    service_name_ = options.service_name;
    minimum_level_ = options.minimum_level;
    mirror_stdout_ = options.mirror_stdout;
    return true;
}

void RunLogger::close() noexcept {
    std::lock_guard lock(mutex_);
    if (stream_.is_open()) {
        stream_.flush();
        stream_.close();
    }
}

void RunLogger::log(LogLevel level, std::string_view message) {
    std::lock_guard lock(mutex_);
    if (!stream_.is_open() || static_cast<int>(level) < static_cast<int>(minimum_level_)) {
        return;
    }
    write_locked(level, message);
}

void RunLogger::write_locked(LogLevel level, std::string_view message) {
    std::ostringstream line;
    line << timestamp(std::chrono::system_clock::now(), true)
         << "|[" << std::setfill('0') << std::setw(5) << process_id()
         << "][" << std::setfill('0') << std::setw(6) << (thread_id() % 1000000)
         << "][" << service_name_ << "][" << log_level_name(level) << "]: " << message;
    stream_ << line.str() << '\n';
    stream_.flush();
    if (mirror_stdout_) {
        std::cout << line.str() << '\n';
        std::cout.flush();
    }
}

void RunLogger::debug(std::string_view message) {
    log(LogLevel::Debug, message);
}

void RunLogger::info(std::string_view message) {
    log(LogLevel::Info, message);
}

void RunLogger::warn(std::string_view message) {
    log(LogLevel::Warn, message);
}

void RunLogger::error(std::string_view message) {
    log(LogLevel::Error, message);
}

bool RunLogger::is_open() const noexcept {
    std::lock_guard lock(mutex_);
    return stream_.is_open();
}

const std::filesystem::path& RunLogger::log_path() const noexcept {
    return log_path_;
}

}  // namespace oxmbs::logging
