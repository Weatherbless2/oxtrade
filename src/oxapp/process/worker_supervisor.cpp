#include "oxapp/process/worker_supervisor.h"

namespace oxapp::process {

WorkerSupervisor::WorkerSupervisor(std::size_t desired_count, WorkerEntry entry)
    : desired_count_(desired_count), entry_(entry) {}  // 只保存配置，实际 Worker 在 start() 中创建。

WorkerSupervisor::~WorkerSupervisor() {
    stop();
}

bool WorkerSupervisor::start() {
    if (started_ || desired_count_ == 0) {  // 已启动或配置为零个 Worker 时拒绝启动。
        return false;
    }

    if (workers_.empty()) {  // 只在首次启动时创建对象，后续重启复用原有逻辑编号。
        for (std::size_t worker_id = 0; worker_id < desired_count_; ++worker_id) {
            workers_.emplace_back(worker_id, entry_);  // 每个 Worker 使用相同入口，但拥有独立 PID。
        }
    }

    for (auto& worker : workers_) {
        if (!worker.start()) {  // 部分启动失败时回收之前已启动的 Worker，避免半启动状态。
            stop();
            return false;
        }
    }

    stopping_ = false;  // 清除上一次停止流程留下的保护标志。
    started_ = true;  // 只有全部 Worker 启动成功才对外报告 supervisor 已启动。
    return true;
}

void WorkerSupervisor::monitor_once() {
    if (!started_ || stopping_) {  // 未启动或正在停止时不做巡检，尤其不能触发自动重启。
        return;
    }

    for (auto& worker : workers_) {
        if (!worker.poll_exit()) {  // Worker 仍运行或没有可回收事件，继续检查下一个。
            continue;
        }

        if (worker.state() == WorkerState::Crashed) {  // 只有异常退出才自动拉起；主动停止不会复活。
            worker.restart();
        }
    }
}

bool WorkerSupervisor::stop(std::chrono::milliseconds timeout) {
    if (!started_) {  // 即使 start() 未完成，也尝试清理已经创建的 Worker 对象。
        bool stopped = true;
        for (auto& worker : workers_) {
            stopped = worker.stop(timeout) && stopped;
        }
        return stopped;
    }

    stopping_ = true;  // 让 monitor_once() 在整个批量停止期间保持静默。
    bool stopped = true;
    for (auto& worker : workers_) {
        stopped = worker.stop(timeout) && stopped;  // 不因单个失败提前返回，尽量回收全部 Worker。
    }

    started_ = false;  // 所有 stop() 调用结束后再更新 supervisor 对外状态。
    stopping_ = false;
    return stopped;
}

std::size_t WorkerSupervisor::desired_count() const noexcept {
    return desired_count_;
}

std::size_t WorkerSupervisor::running_count() const noexcept {
    std::size_t count = 0;
    for (const auto& worker : workers_) {  // 以状态和 PID 双重条件避免把已退出对象算作运行中。
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
    if (worker_id >= workers_.size()) {  // 越界查询返回空指针，由调用方决定如何处理。
        return nullptr;
    }

    return &workers_[worker_id];
}

}  // namespace oxapp::process
