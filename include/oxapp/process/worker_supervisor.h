#pragma once

#include "oxapp/process/worker_process.h"

#include <chrono>
#include <cstddef>
#include <deque>

namespace oxapp::process {

class WorkerSupervisor {
public:
    explicit WorkerSupervisor(std::size_t desired_count = 2,
                              WorkerEntry entry = nullptr);
    ~WorkerSupervisor();

    WorkerSupervisor(const WorkerSupervisor&) = delete;
    WorkerSupervisor& operator=(const WorkerSupervisor&) = delete;

    bool start();
    void monitor_once();
    bool stop(std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

    std::size_t desired_count() const noexcept;
    std::size_t running_count() const noexcept;
    bool started() const noexcept;
    const WorkerProcess* worker(std::size_t worker_id) const noexcept;

private:
    std::size_t desired_count_;
    WorkerEntry entry_;
    std::deque<WorkerProcess> workers_;
    bool started_{false};
    bool stopping_{false};
};

}  // namespace oxapp::process
