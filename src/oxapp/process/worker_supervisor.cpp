#include "oxapp/process/worker_supervisor.h"

namespace oxapp::process {

WorkerSupervisor::WorkerSupervisor(std::size_t desired_count, WorkerEntry entry)
    : desired_count_(desired_count), entry_(entry) {}

WorkerSupervisor::~WorkerSupervisor() {
    stop();
}

bool WorkerSupervisor::start() {
    if (started_ || desired_count_ == 0) {
        return false;
    }

    if (workers_.empty()) {
        for (std::size_t worker_id = 0; worker_id < desired_count_; ++worker_id) {
            workers_.emplace_back(worker_id, entry_);
        }
    }

    for (auto& worker : workers_) {
        if (!worker.start()) {
            stop();
            return false;
        }
    }

    stopping_ = false;
    started_ = true;
    return true;
}

void WorkerSupervisor::monitor_once() {
    if (!started_ || stopping_) {
        return;
    }

    for (auto& worker : workers_) {
        if (!worker.poll_exit()) {
            continue;
        }

        if (worker.state() == WorkerState::Crashed) {
            worker.restart();
        }
    }
}

bool WorkerSupervisor::stop(std::chrono::milliseconds timeout) {
    if (!started_) {
        bool stopped = true;
        for (auto& worker : workers_) {
            stopped = worker.stop(timeout) && stopped;
        }
        return stopped;
    }

    stopping_ = true;
    bool stopped = true;
    for (auto& worker : workers_) {
        stopped = worker.stop(timeout) && stopped;
    }

    started_ = false;
    stopping_ = false;
    return stopped;
}

std::size_t WorkerSupervisor::desired_count() const noexcept {
    return desired_count_;
}

std::size_t WorkerSupervisor::running_count() const noexcept {
    std::size_t count = 0;
    for (const auto& worker : workers_) {
        if (worker.state() == WorkerState::Running && worker.pid() > 0) {
            ++count;
        }
    }
    return count;
}

bool WorkerSupervisor::started() const noexcept {
    return started_;
}

const WorkerProcess* WorkerSupervisor::worker(std::size_t worker_id) const noexcept {
    if (worker_id >= workers_.size()) {
        return nullptr;
    }

    return &workers_[worker_id];
}

}  // namespace oxapp::process
