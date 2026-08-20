#pragma once

#include "oxmbs/ipc/systemv_message_queue.h"
#include "oxmbs/ipc/task_correlation.h"
#include "oxmbs/ipc/task_protocol.h"

#include <cstddef>
#include <string>
#include <vector>

namespace oxmbs::logging {
class RunLogger;
}

namespace oxmbs::worker {

using ResponseSink = void (*)(const ipc::IpcRespMsg& response, void* context);  // 主进程对接 HP-Socket 的预留回调；Mock 测试用它收集响应，不把网络对象交给 Worker。
using TimeoutSink = void (*)(const ipc::ExpiredTask& task, void* context);  // pending 超时后的上游通知预留；#7 不定义真实网络错误包。

enum class ReplyCollectorStepStatus {
    DeliveredToSink,       // Response 解码和关联都成功，已交给主进程 Mock/未来 HP-Socket sink。
    ResponseSinkFailed,    // 关联已成功消费，但主进程回调抛出异常；记录后 Collector 继续，不能重复投递同一 Reply。
    InvalidResponseDropped,// Queue 中的帧已取走但不能按 v2 Response 解码，不能进入客户端回包路径。
    CorrelationRejected,   // task_seq/session/deadline 未精确匹配，禁止误回包；result 中保留关联拒绝原因。
    ReplyInterrupted,      // 阻塞 msgrcv 被信号打断；run() 可继续收取。
    ReplyQueueRemoved,     // 主进程关闭队列，Collector 正常停止。
    ReplyReceiveFailed,    // 其他接收错误；run() 返回非零让上层处理。
};

const char* reply_collector_step_status_name(ReplyCollectorStepStatus status) noexcept;

struct ReplyCollectorOptions {
    ipc::TaskProtocolLimits protocol_limits{};  // 必须与 Worker 和路由使用同一 Task/Response 最大帧限制。
    long reply_message_type{2};  // 只收 Worker 应答的 System V mtype；业务类型仍读取协议内 msg_type。
};

struct ReplyCollectorStepResult {
    ReplyCollectorStepStatus status{ReplyCollectorStepStatus::ReplyReceiveFailed};
    ipc::TaskSequence task_seq{0};
    ipc::TaskCorrelationStatus correlation_status{ipc::TaskCorrelationStatus::InvalidArgument};
    ipc::IpcResult ipc_result{ipc::IpcResult::Ok};
    std::string error;
};

class ReplyCollector {
public:
    ReplyCollector(ipc::SystemVMessageQueue& reply_queue,
                   ipc::TaskCorrelationRegistry& correlation_registry,
                   ReplyCollectorOptions options = {},
                   ResponseSink response_sink = nullptr,
                   void* response_sink_context = nullptr,
                   TimeoutSink timeout_sink = nullptr,
                   void* timeout_sink_context = nullptr,
                   logging::RunLogger* logger = nullptr);

    ReplyCollectorStepResult collect_one();  // 主进程阻塞接收一条 Reply，精确关联后才调用 response_sink_。
    std::vector<ipc::ExpiredTask> expire_pending();  // 定时器调用：删除主进程本地超时记录，并将快照交给可选 timeout sink。
    int run();  // 连续执行 collect_one()；队列关闭正常结束，其他不可恢复接收错误返回非零。

private:
    bool options_are_valid() const noexcept;
    void log_debug(const std::string& message) const;
    void log_warning(const std::string& message) const;
    void log_error(const std::string& message) const;

    ipc::SystemVMessageQueue& reply_queue_;  // 只借用主进程队列，ReplyCollector 不拥有 IPC_RMID 权限。
    ipc::TaskCorrelationRegistry& correlation_registry_;  // 只在主进程使用；Worker 不访问此表或网络 session。
    ReplyCollectorOptions options_;
    ResponseSink response_sink_{nullptr};
    void* response_sink_context_{nullptr};
    TimeoutSink timeout_sink_{nullptr};
    void* timeout_sink_context_{nullptr};
    logging::RunLogger* logger_{nullptr};
    ipc::IpcPayload receive_buffer_;  // 固定上限的复用接收空间，解码成功前不构造半成品 Response。
};

}  // namespace oxmbs::worker
