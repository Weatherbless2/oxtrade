#include "oxmbs/ipc/task_correlation.h"

#include <limits>

namespace oxmbs::ipc {

TaskRegistration TaskCorrelationRegistry::register_request(SessionId session_id,
                                                            std::chrono::milliseconds timeout,
                                                            Clock::time_point now) {
    TaskRegistration registration;
    if (session_id == 0 || timeout <= std::chrono::milliseconds::zero()) {
        registration.status = TaskCorrelationStatus::InvalidArgument;
        return registration;
    }

    TaskSequence task_seq = 0;
    TaskSequence expected = next_task_seq_.load(std::memory_order_relaxed);
    while (true) {
        if (expected == 0 || expected == std::numeric_limits<TaskSequence>::max()) {
            registration.status = TaskCorrelationStatus::SequenceExhausted;  // 到达上限后宁可拒绝新任务，也不能回绕复用旧 task_seq 错投结果。
            return registration;
        }
        const TaskSequence next = expected + 1;
        if (next_task_seq_.compare_exchange_weak(expected, next, std::memory_order_relaxed)) {
            task_seq = expected;  // CAS 成功的线程独占这个序号；失败时 expected 会刷新为当前值并继续竞争。
            break;
        }
    }

    const PendingTask pending{session_id, now + timeout};
    {
        std::lock_guard<std::mutex> lock(mutex_);  // 注册、应答回收和超时清理共享同一张表，必须把插入与查找/删除串行化。
        pending_.emplace(task_seq, pending);
    }
    registration.status = TaskCorrelationStatus::Registered;
    registration.task_seq = task_seq;
    registration.session_id = session_id;
    return registration;
}

ResponseCorrelation TaskCorrelationRegistry::resolve_response(const IpcRespMsg& response, Clock::time_point now) {
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
    if (now >= found->second.deadline) {
        pending_.erase(found);  // 到期结果不能再回给客户端；删除记录也防止迟到应答长期占用关联表。
        correlation.status = TaskCorrelationStatus::TimedOut;
        return correlation;
    }
    if (response.session_id != found->second.session_id) {
        correlation.status = TaskCorrelationStatus::SessionMismatch;  // 保留原待处理记录，允许真正对应会话的应答稍后正常到达。
        return correlation;
    }

    correlation.session_id = found->second.session_id;
    pending_.erase(found);  // 精确匹配后立即消费，确保同一 task_seq 的重复应答会被标记为 UnknownTask。
    correlation.status = TaskCorrelationStatus::Matched;
    return correlation;
}

std::vector<ExpiredTask> TaskCorrelationRegistry::expire(Clock::time_point now) {
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
    case TaskCorrelationStatus::TimedOut:
        return "TimedOut";
    case TaskCorrelationStatus::SequenceExhausted:
        return "SequenceExhausted";
    }
    return "Unknown";
}

}  // namespace oxmbs::ipc
