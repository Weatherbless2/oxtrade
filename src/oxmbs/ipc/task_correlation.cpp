#include "oxmbs/ipc/task_correlation.h"

#include <limits>

namespace oxmbs::ipc {

TaskRegistration TaskCorrelationRegistry::register_request(SessionId session_id,
                                                            std::chrono::milliseconds timeout,
                                                            Clock::time_point now,
                                                            WallClock::time_point wall_now) {
    TaskRegistration registration;  // 默认失败结果；只有序号分配和 pending_ 插入完成后才发布 Registered。
    if (session_id == 0 || timeout <= std::chrono::milliseconds::zero()) {
        registration.status = TaskCorrelationStatus::InvalidArgument;
        return registration;
    }

    const auto wall_deadline = wall_now + timeout;  // Worker 不共享 pending_，因此同一超时必须先转换为绝对墙上时间写进协议帧。
    const auto deadline_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(wall_deadline.time_since_epoch()).count();
    if (deadline_unix_ms <= 0 || static_cast<std::uintmax_t>(deadline_unix_ms) > std::numeric_limits<std::uint64_t>::max()) {
        registration.status = TaskCorrelationStatus::InvalidArgument;  // 非法或不可表示的墙上时间不允许生成“永不过期”的 Task。
        return registration;
    }

    TaskSequence task_seq = 0;
    // 序号只要求唯一递增，不承担跨变量的可见性同步；真正的关联记录由后面的 mutex_ 保护，因此这里使用 relaxed 原子操作。
    TaskSequence expected = next_task_seq_.load(std::memory_order_relaxed);
    while (true) {
        if (expected == 0 || expected == std::numeric_limits<TaskSequence>::max()) {
            registration.status = TaskCorrelationStatus::SequenceExhausted;  // 到达上限后宁可拒绝新任务，也不能回绕复用旧 task_seq 错投结果。
            return registration;
        }
        const TaskSequence next = expected + 1;  // 先计算候选下一个值，CAS 成功前不修改共享计数器。
        if (next_task_seq_.compare_exchange_weak(expected, next, std::memory_order_relaxed)) {
            task_seq = expected;  // CAS 成功的线程独占这个序号；失败时 expected 会刷新为当前值并继续竞争。
            break;
        }
    }

    const auto wire_deadline = static_cast<std::uint64_t>(deadline_unix_ms);  // 通过范围检查后再缩窄为稳定的无符号协议类型。
    const PendingTask pending{session_id, now + timeout, wire_deadline};  // 主进程本地 steady deadline 与跨进程 wire deadline 来自同一次注册。
    {
        std::lock_guard<std::mutex> lock(mutex_);  // 注册、应答回收和超时清理共享同一张表，必须把插入与查找/删除串行化。
        pending_.emplace(task_seq, pending);
    }
    registration.status = TaskCorrelationStatus::Registered;
    registration.task_seq = task_seq;
    registration.session_id = session_id;
    registration.deadline_unix_ms = wire_deadline;  // 与 pending_ 使用同一 timeout 生成，调用方无需重新计算。
    return registration;
}

ResponseCorrelation TaskCorrelationRegistry::resolve_response(const IpcRespMsg& response, Clock::time_point now) {
    // Reply 只在主进程匹配：Worker 透传 session_id，不能访问网络会话，也不共享 pending_ 内存。
    ResponseCorrelation correlation;
    correlation.task_seq = response.task_seq;
    correlation.session_id = response.session_id;
    if (response.task_seq == 0 || response.session_id == 0) {
        correlation.status = TaskCorrelationStatus::InvalidArgument;
        return correlation;
    }

    std::lock_guard<std::mutex> lock(mutex_);  // 查找、会话比对和最终 erase 必须处在同一临界区，避免两个 Reply 线程重复回包。
    const auto found = pending_.find(response.task_seq);
    if (found == pending_.end()) {
        correlation.status = TaskCorrelationStatus::UnknownTask;
        return correlation;
    }
    if (now >= found->second.deadline) {  // 边界时刻算超时，保证 resolve_response() 与 expire() 对同一时刻的结论一致。
        pending_.erase(found);  // 到期结果不能再回给客户端；删除记录也防止迟到应答长期占用关联表。
        correlation.status = TaskCorrelationStatus::TimedOut;
        return correlation;
    }
    if (response.session_id != found->second.session_id) {
        correlation.status = TaskCorrelationStatus::SessionMismatch;  // 保留原待处理记录，允许真正对应会话的应答稍后正常到达。
        return correlation;
    }
    if (response.deadline_unix_ms != found->second.deadline_unix_ms) {
        correlation.status = TaskCorrelationStatus::DeadlineMismatch;  // 截止时间不能由 Worker 延长或改写，错帧不消费正确任务。
        return correlation;
    }

    correlation.session_id = found->second.session_id;
    pending_.erase(found);  // 精确匹配后立即消费，确保同一 task_seq 的重复应答会被标记为 UnknownTask。
    correlation.status = TaskCorrelationStatus::Matched;
    return correlation;
}

std::vector<ExpiredTask> TaskCorrelationRegistry::expire(Clock::time_point now) {
    // 定时器调用本函数后拿到“已删除”的任务快照；registry 不直接发网络超时包，保持协议层与上游网络层解耦。
    std::vector<ExpiredTask> expired;
    std::lock_guard<std::mutex> lock(mutex_);  // 定时清理与应答回收互斥，避免刚匹配的任务又被报告为超时。
    for (auto iterator = pending_.begin(); iterator != pending_.end();) {
        if (now >= iterator->second.deadline) {
            expired.push_back(ExpiredTask{iterator->first, iterator->second.session_id});
            iterator = pending_.erase(iterator);  // erase 返回下一个有效迭代器，保证遍历中删除不会使用失效引用。
        } else {
            ++iterator;
        }
    }
    return expired;
}

std::size_t TaskCorrelationRegistry::pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);  // 监控指标读取也加锁，避免 unordered_map 并发读写造成未定义行为。
    return pending_.size();
}

const char* task_correlation_status_name(TaskCorrelationStatus status) noexcept {
    switch (status) {
    case TaskCorrelationStatus::Registered:
        return "Registered";
    case TaskCorrelationStatus::Matched:
        return "Matched";
    case TaskCorrelationStatus::InvalidArgument:
        return "InvalidArgument";
    case TaskCorrelationStatus::UnknownTask:
        return "UnknownTask";
    case TaskCorrelationStatus::SessionMismatch:
        return "SessionMismatch";
    case TaskCorrelationStatus::DeadlineMismatch:
        return "DeadlineMismatch";
    case TaskCorrelationStatus::TimedOut:
        return "TimedOut";
    case TaskCorrelationStatus::SequenceExhausted:
        return "SequenceExhausted";
    }
    return "Unknown";
}

}  // namespace oxmbs::ipc
