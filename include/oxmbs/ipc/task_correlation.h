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
    Registered,        // 请求已拿到唯一 task_seq，并已写入主进程 pending_ 表。
    Matched,           // Reply 的 task_seq 和 session_id 都匹配，记录已被消费。
    InvalidArgument,   // 零会话、非正超时或零关联字段等输入不具备关联意义。
    UnknownTask,       // 未找到 task_seq，通常是重复 Reply、迟到清理或非本进程生成的帧。
    SessionMismatch,   // task_seq 存在但 Reply 会话不同，禁止把结果投递给错误客户端。
    DeadlineMismatch,  // task_seq/session_id 正确但 deadline 被篡改；保留 pending_，等待真正对应的 Reply。
    TimedOut,          // 关联记录已到截止时间，本次 Reply 被拒绝并删除记录。
    SequenceExhausted, // 64 位序号到达边界；拒绝新请求，绝不回绕复用旧编号。
};

struct TaskRegistration {
    TaskCorrelationStatus status{TaskCorrelationStatus::InvalidArgument};
    TaskSequence task_seq{0};
    SessionId session_id{0};
    std::uint64_t deadline_unix_ms{0};  // 同一次注册生成的跨进程绝对截止时间；只有 Registered 时调用方才可编码进 Task。

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
    using WallClock = std::chrono::system_clock;

    TaskRegistration register_request(SessionId session_id,
                                      std::chrono::milliseconds timeout,
                                      Clock::time_point now = Clock::now(),
                                      WallClock::time_point wall_now = WallClock::now());  // 用 steady_clock 保护主进程本地超时，同时生成可序列化的 wall-clock deadline 给 Worker。
    ResponseCorrelation resolve_response(const IpcRespMsg& response,
                                         Clock::time_point now = Clock::now());
                                         // 匹配成功即删除；错会话保留记录；未知/超时 Reply 不会被投递。
    std::vector<ExpiredTask> expire(Clock::time_point now = Clock::now());  // 定时取得并删除所有到期任务，上层决定如何回超时结果。
    std::size_t pending_count() const;  // 受锁保护的观测值，用于测试或监控，不能绕过 registry 直接访问 pending_。

private:
    struct PendingTask {
        SessionId session_id{0};
        Clock::time_point deadline{};  // 使用 steady_clock，避免系统时间校准改变相对超时语义。
        std::uint64_t deadline_unix_ms{0};  // 同时保留 Task wire 字段，Reply 回收时验证 Worker 没有改写截止时间。
    };

    std::atomic<TaskSequence> next_task_seq_{1};  // 仅主进程持有一个 registry，原子递增保证多个收包线程注册时序号不重复。
    mutable std::mutex mutex_;  // pending_ 同时由 WorkFunc 注册、ReplyCollectThread 回收和定时清理线程访问。
    std::unordered_map<TaskSequence, PendingTask> pending_;  // task_seq -> 预期会话和截止时刻，不在 Worker 进程之间共享。
};

const char* task_correlation_status_name(TaskCorrelationStatus status) noexcept;

}  // namespace oxmbs::ipc
