#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace oxapp::process {

using ProcessId = std::int64_t;
using WorkerEntry = int (*)();

enum class WorkerState {
    Created,
    Starting,
    Running,
    Stopping,
    Stopped,
    Crashed,
    Restarting,
};

const char* worker_state_name(WorkerState state) noexcept;

class WorkerProcess {
public:
    explicit WorkerProcess(std::size_t worker_id, WorkerEntry entry = nullptr);
    ~WorkerProcess();

    WorkerProcess(const WorkerProcess&) = delete;
    WorkerProcess& operator=(const WorkerProcess&) = delete;
    WorkerProcess(WorkerProcess&&) = delete;
    WorkerProcess& operator=(WorkerProcess&&) = delete;

    bool start();
    bool poll_exit();
    bool restart();
    bool stop(std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

    std::size_t worker_id() const noexcept;
    ProcessId pid() const noexcept;
    WorkerState state() const noexcept;
    std::size_t restart_count() const noexcept;
    std::chrono::system_clock::time_point start_time() const noexcept;
    bool has_exit_status() const noexcept;
    int last_exit_code() const noexcept;
    int last_exit_signal() const noexcept;

private:
    bool launch();
    void record_exit(int status) noexcept;

    std::size_t worker_id_;
    WorkerEntry entry_;
    ProcessId pid_{-1};
    WorkerState state_{WorkerState::Created};
    std::size_t restart_count_{0};
    std::chrono::system_clock::time_point start_time_{};
    bool has_exit_status_{false};
    int last_exit_code_{-1};
    int last_exit_signal_{0};
};

}  // namespace oxapp::process
