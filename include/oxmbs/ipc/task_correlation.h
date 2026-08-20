#pragma once

#include "oxmbs/ipc/task_protocol.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace oxmbs::ipc {

enum class TaskCorrelationStatus {
    Registered,
    Matched,
    InvalidArgument,
    UnknownTask,
    SessionMismatch,
    TimedOut,
    SequenceExhausted,
};

struct TaskRegistration {
    TaskCorrelationStatus status{TaskCorrelationStatus::InvalidArgument};
    TaskSequence task_seq{0};
    SessionId session_id{0};

    explicit operator bool() const noexcept { return status == TaskCorrelationStatus::Registered; }
};

struct ResponseCorrelation {
    TaskCorrelationStatus status{TaskCorrelationStatus::InvalidArgument};
    TaskSequence task_seq{0};
    SessionId session_id{0};

    explicit operator bool() const noexcept { return status == TaskCorrelationStatus::Matched; }
};

struct ExpiredTask {
    TaskSequence task_seq{0};
    SessionId session_id{0};
};

class TaskCorrelationRegistry {
public:
    using Clock = std::chrono::steady_clock;

    TaskRegistration register_request(SessionId session_id,
                                      std::chrono::milliseconds timeout,
                                      Clock::time_point now = Clock::now());
    ResponseCorrelation resolve_response(const IpcRespMsg& response,
                                         Clock::time_point now = Clock::now());
    std::vector<ExpiredTask> expire(Clock::time_point now = Clock::now());
    std::size_t pending_count() const;

private:
    struct PendingTask {
        SessionId session_id{0};
        Clock::time_point deadline{};
    };

    std::atomic<TaskSequence> next_task_seq_{1};  // 仅主进程持有一个 registry，原子递增保证多个收包线程注册时序号不重复。
    mutable std::mutex mutex_;  // pending_ 同时由 WorkFunc 注册、ReplyCollectThread 回收和定时清理线程访问。
    std::unordered_map<TaskSequence, PendingTask> pending_;  // task_seq -> 预期会话和截止时刻，不在 Worker 进程之间共享。
};

const char* task_correlation_status_name(TaskCorrelationStatus status) noexcept;

}  // namespace oxmbs::ipc
