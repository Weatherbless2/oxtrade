#pragma once

#include "oxmbs/ipc/systemv_message_queue.h"
#include "oxmbs/ipc/task_protocol.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace oxmbs::logging {
class RunLogger;
}

namespace oxmbs::worker {

struct MockQueryExecutionResult {
    ipc::IpcResponseStatus status{ipc::IpcResponseStatus::Ok};  // Mock 入口的业务结果；#8 LBM 适配后仍需映射到同一 Response 枚举。
    ipc::IpcPayload payload;  // 成功数据或错误说明的原始字节；Worker 不假定 JSON、文本或数据库行结构。
};

using QueryTaskHandler = MockQueryExecutionResult (*)(const ipc::IpcTaskMsg& task,
                                                       void* context);  // 暂定 C 风格业务入口和上下文；#8 可由 LBM 函数指针/模块上下文适配替换。

enum class QueryWorkerStepStatus {
    ResponseSent,          // 未过期任务已执行，Response 成功写入 Reply Queue。
    TaskExpiredDropped,    // Worker 自己发现 deadline 已过期；不执行业务也不写成功 Response。
    InvalidTaskDropped,    // Request Queue 已取走损坏或不兼容帧，记录后丢弃，避免坏帧阻塞后续任务。
    RequestInterrupted,    // 阻塞 msgrcv 被信号打断；run() 可继续接收下一条任务。
    RequestQueueRemoved,   // 主进程删除 Request Queue，Worker 正常结束循环；Worker 自身不删除队列。
    RequestReceiveFailed,  // 其他接收错误；run() 退出并让 Supervisor 按异常处理。
    ResponseEncodingFailed,// 业务结果无法编码为当前 Response 协议；请求已消费，记录后不重试。
    ReplyQueueRemoved,     // 主进程删除 Reply Queue，Worker 正常结束循环。
    ReplySendFailed,       // Reply 写回失败；run() 退出，避免连续失败造成无意义忙循环。
};

const char* query_worker_step_status_name(QueryWorkerStepStatus status) noexcept;

struct QueryWorkerRuntimeOptions {
    ipc::TaskProtocolLimits protocol_limits{};  // 与 #5 编码和 #6 路由使用相同最大帧限制，接收 buffer 不依赖固定 char 数组。
    long request_message_type{1};  // Request Queue 的 System V mtype；仅为内核筛选，不等同于业务 msg_type。
    long reply_message_type{2};  // Reply Queue 的 System V mtype；与 Request type 分离，便于主进程应答回收。
};

struct QueryWorkerStepResult {
    QueryWorkerStepStatus status{QueryWorkerStepStatus::RequestReceiveFailed};
    ipc::TaskSequence task_seq{0};  // 解码成功后记录，帮助日志和监控定位单个任务。
    ipc::IpcResponseStatus response_status{ipc::IpcResponseStatus::Ok};  // 仅 ResponseSent 时表示写回的 Worker 业务状态。
    ipc::IpcResult ipc_result{ipc::IpcResult::Ok};  // 收发队列的系统语义，例如 Interrupted、QueueRemoved、WouldBlock。
    std::string error;
};

class QueryWorkerRuntime {
public:
    QueryWorkerRuntime(ipc::SystemVMessageQueue& request_queue,
                       ipc::SystemVMessageQueue& reply_queue,
                       QueryWorkerRuntimeOptions options = {},
                       QueryTaskHandler handler = nullptr,
                       void* handler_context = nullptr,
                       logging::RunLogger* logger = nullptr);

    QueryWorkerStepResult consume_one();  // 阻塞取一条 Request；调用者仅在 Worker 子进程中执行，主进程不调用此方法消费查询队列。
    int run();  // 持续阻塞消费；Request/Reply Queue 被主进程删除时正常返回 0，其他不可恢复 IPC 错误返回非零供 Supervisor 重启。

private:
    static MockQueryExecutionResult echo_mock_handler(const ipc::IpcTaskMsg& task,
                                                       void* context);  // 默认 Mock 行为：原样回显 payload，证明协议闭环；不代表真实登录或查询业务。
    bool options_are_valid() const noexcept;
    std::uint64_t now_unix_ms() const noexcept;
    void log_debug(const std::string& message) const;
    void log_warning(const std::string& message) const;
    void log_error(const std::string& message) const;

    ipc::SystemVMessageQueue& request_queue_;  // 只借用父进程创建的队列；Worker 永远不调用 remove()/IPC_RMID。
    ipc::SystemVMessageQueue& reply_queue_;  // 同样只借用，主进程负责关闭时唤醒阻塞 Worker。
    QueryWorkerRuntimeOptions options_;
    QueryTaskHandler handler_{nullptr};  // 空时使用 echo_mock_handler；#8 通过 LBM 适配替换，保持接收/超时/回包逻辑不变。
    void* handler_context_{nullptr};  // 可选 Mock/未来 LBM 上下文；必须可安全 fork 后读取，Worker 只借用，不释放或改变对象所有权。
    logging::RunLogger* logger_{nullptr};  // Worker 在网络线程启动前 fork，使用共享 run 日志记录消费、丢弃和失败路径。
    ipc::IpcPayload receive_buffer_;  // 每个 Worker 进程复用固定上限 buffer，避免每条低频任务都重新分配接收空间。
};

int query_worker_runtime_entry(void* context);  // 传给 #2 WorkerSupervisor 的 C 风格入口；context 必须是仍存活的 QueryWorkerRuntime。

}  // namespace oxmbs::worker
