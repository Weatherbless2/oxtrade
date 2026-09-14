#include "oxmbs/worker/query_worker_runtime.h"

#include "oxmbs/logging/run_logger.h"

#include <chrono>
#include <exception>
#include <limits>
#include <utility>

namespace oxmbs::worker {
namespace {

std::string task_context(const ipc::IpcTaskMsg& task) {
    return "task_seq=" + std::to_string(task.task_seq) +
           " session_id=" + std::to_string(task.session_id) +
           " msg_type=" + std::to_string(task.msg_type) +
           " func_id=" + std::to_string(task.func_id);
}

}  // namespace

const char* query_worker_step_status_name(QueryWorkerStepStatus status) noexcept {
    switch (status) {
    case QueryWorkerStepStatus::ResponseSent:
        return "ResponseSent";
    case QueryWorkerStepStatus::TaskExpiredDropped:
        return "TaskExpiredDropped";
    case QueryWorkerStepStatus::InvalidTaskDropped:
        return "InvalidTaskDropped";
    case QueryWorkerStepStatus::RequestInterrupted:
        return "RequestInterrupted";
    case QueryWorkerStepStatus::RequestQueueRemoved:
        return "RequestQueueRemoved";
    case QueryWorkerStepStatus::RequestReceiveFailed:
        return "RequestReceiveFailed";
    case QueryWorkerStepStatus::ResponseEncodingFailed:
        return "ResponseEncodingFailed";
    case QueryWorkerStepStatus::ReplyQueueRemoved:
        return "ReplyQueueRemoved";
    case QueryWorkerStepStatus::ReplySendFailed:
        return "ReplySendFailed";
    }
    return "Unknown";
}

QueryWorkerRuntime::QueryWorkerRuntime(ipc::SystemVMessageQueue& request_queue,
                                       ipc::SystemVMessageQueue& reply_queue,
                                       QueryWorkerRuntimeOptions options,
                                       QueryTaskHandler handler,
                                       void* handler_context,
                                       logging::RunLogger* logger,
                                       QueryWorkerStartHook start_hook,
                                       void* start_context)
    : request_queue_(request_queue),
      reply_queue_(reply_queue),
      options_(options),
      handler_(handler != nullptr ? handler : &QueryWorkerRuntime::echo_mock_handler),
      handler_context_(handler_context),
      logger_(logger),
      start_hook_(start_hook),
      start_context_(start_context),
      receive_buffer_(options.protocol_limits.max_frame_bytes) {
}

QueryWorkerStepResult QueryWorkerRuntime::consume_one() {
    QueryWorkerStepResult result;  // 每次循环只保存一条任务的处理结果，run() 据此决定继续、退出或让 Supervisor 重启。
    if (!options_are_valid()) {  // 先检查队列类型、协议帧上限和接收缓冲区，避免带着错误边界进入 msgrcv。
        result.status = QueryWorkerStepStatus::RequestReceiveFailed;  // 配置错误属于不可恢复的 Worker 启动问题。
        result.ipc_result = ipc::IpcResult::InvalidArgument;  // 用稳定 IPC 状态代替底层参数错误细节。
        result.error = "QueryWorkerRuntime options are invalid";  // 主进程日志可据此定位 Worker 配置。
        log_error(result.error);  // 记录后返回，由 run() 返回非零并交给 Supervisor 处理。
        return result;  // 不创建应答，也不继续读取未知大小的请求。
    }

    const auto received = request_queue_.receive(
        options_.request_message_type, receive_buffer_.data(), receive_buffer_.size(), false);  // false 使同质 Worker 在空队列休眠，由内核唤醒一个消费者。
    result.ipc_result = received.result;  // 保存 msgrcv 的业务化结果，让调用方区分中断、删除和真正失败。
    if (!received) {
        result.error = received.error;  // 保留 errno 映射后的可读原因，后续日志不重新猜测。
        switch (received.result) {
        case ipc::IpcResult::Interrupted:
            result.status = QueryWorkerStepStatus::RequestInterrupted;
            log_debug("query worker request receive interrupted");
            return result;
        case ipc::IpcResult::QueueRemoved:
            result.status = QueryWorkerStepStatus::RequestQueueRemoved;
            log_debug("query worker request queue removed; stopping without deleting IPC");
            return result;
        default:
            result.status = QueryWorkerStepStatus::RequestReceiveFailed;
            log_error("query worker request receive failed ipc_result=" +
                      std::string(ipc::ipc_result_name(received.result)) + " error=" + received.error);
            return result;
        }
    }

    const auto decoded = ipc::decode_task(receive_buffer_.data(), received.size, options_.protocol_limits);  // 只按 msgrcv 实际长度解码，避免把旧 buffer 尾部当成新字段。
    if (!decoded) {
        result.status = QueryWorkerStepStatus::InvalidTaskDropped;
        result.error = decoded.error;
        log_warning("query worker dropped invalid request frame protocol_status=" +
                    std::string(ipc::task_protocol_status_name(decoded.status)) + " error=" + decoded.error);  // msgrcv 已消费坏帧，记录后继续不会阻塞后续合法任务。
        return result;
    }

    const auto& task = decoded.message;  // 解码对象引用当前 buffer 对应的结构化任务，不复制 payload。
    result.task_seq = task.task_seq;  // 先保存序号，即使后面超时或业务失败，日志也能定位这条任务。
    if (task.deadline_unix_ms <= now_unix_ms()) {  // 先检查跨进程绝对 deadline；过期任务不能调用 Mock 业务，更不能伪造成功 Reply。
        result.status = QueryWorkerStepStatus::TaskExpiredDropped;
        log_warning("query worker dropped expired request " + task_context(task));
        return result;
    }

    MockQueryExecutionResult execution;
    try {
        execution = handler_(task, handler_context_);  // deadline 已通过后才进入 Dispatcher/LBM；返回统一 status 和自有 payload。
    } catch (const std::exception& exception) {
        execution.status = ipc::IpcResponseStatus::Failed;
        const std::string message = std::string("mock query handler exception: ") + exception.what();
        execution.payload.assign(message.begin(), message.end());  // 业务异常也要生成 Failed Reply，让主进程能精确清理关联而非等待超时。
        log_warning("query worker handler exception " + task_context(task) + " error=" + exception.what());
    } catch (...) {
        execution.status = ipc::IpcResponseStatus::Failed;
        const std::string message = "mock query handler unknown exception";
        execution.payload.assign(message.begin(), message.end());
        log_warning("query worker handler unknown exception " + task_context(task));
    }

    ipc::IpcRespMsg response;  // 新建应答对象，不能复用 Request 的可借用输入内存。
    response.task_seq = task.task_seq;  // 原样回传任务序号，ReplyCollector 依靠它完成关联。
    response.session_id = task.session_id;  // 原样回传会话号，避免不同会话串包。
    response.deadline_unix_ms = task.deadline_unix_ms;  // 带回原 deadline，接收方可判断应答是否仍有意义。
    response.msg_type = task.msg_type;  // 回包保留业务消息类型，便于上游按原协议分派。
    response.func_id = task.func_id;  // 回包保留函数编号，便于诊断具体业务入口。
    response.status = execution.status;  // LBM 的 Ok/Rejected/Failed 已在 Dispatcher 转成 IPC 状态。
    response.payload = std::move(execution.payload);  // 只移动 Worker 已拥有的业务结果，Request payload 不再引用主进程 buffer。

    ipc::IpcPayload frame;
    std::string error;
    const auto encoding_result = ipc::encode_response(response, frame, error, options_.protocol_limits);  // 把结构化应答写成受限长度的 IPC 字节帧。
    if (encoding_result != ipc::TaskProtocolStatus::Ok) {
        result.status = QueryWorkerStepStatus::ResponseEncodingFailed;
        result.error = error;
        log_error("query worker response encoding failed " + task_context(task) + " protocol_status=" +
                  ipc::task_protocol_status_name(encoding_result) + " error=" + error);
        return result;
    }

    const auto send_result = reply_queue_.send(
        options_.reply_message_type, frame.data(), frame.size(), false, error);  // Worker 可阻塞等待 Reply Queue 空间；这条低频路径不会阻塞 WorkFunc 或交易/行情链路。
    result.ipc_result = send_result;  // 保存 msgsnd 的业务化结果，run() 决定是正常退出还是异常退出。
    if (send_result != ipc::IpcResult::Ok) {
        result.error = error;
        result.status = send_result == ipc::IpcResult::QueueRemoved
                            ? QueryWorkerStepStatus::ReplyQueueRemoved
                            : QueryWorkerStepStatus::ReplySendFailed;
        log_error("query worker reply send failed " + task_context(task) + " ipc_result=" +
                  ipc::ipc_result_name(send_result) + " error=" + error);
        return result;
    }

    result.status = QueryWorkerStepStatus::ResponseSent;  // 只有 msgsnd 成功后才允许把本次任务标记为已完成。
    result.response_status = response.status;  // 把实际业务状态交给监控和测试读取。
    log_debug("query worker response sent " + task_context(task));  // 日志中的 task/session/msg/func 可与主进程回包对应。
    return result;  // 返回 run()，下一轮继续消费下一条 Request。
}

int QueryWorkerRuntime::run() {
    if (start_hook_ != nullptr) {
        std::string error;
        if (!start_hook_(start_context_, error)) {
            log_error("query worker start hook failed: " + error);  // 子进程资源初始化失败时退出，由 Supervisor 统一判定和告警。
            return 1;
        }
    }
    // 主进程在网络监听前 fork Worker；每个 Worker 只在此循环阻塞读取 Request Queue，不持有 session 或数据库连接。
    while (true) {
        const auto result = consume_one();
        switch (result.status) {
        case QueryWorkerStepStatus::ResponseSent:
        case QueryWorkerStepStatus::TaskExpiredDropped:
        case QueryWorkerStepStatus::InvalidTaskDropped:
        case QueryWorkerStepStatus::RequestInterrupted:
        case QueryWorkerStepStatus::ResponseEncodingFailed:
            continue;  // 单个任务/帧失败不应杀死整个 Worker，后续合法任务仍可被同质池消费。
        case QueryWorkerStepStatus::RequestQueueRemoved:  // 主进程删除 Request 会唤醒阻塞 receive，这是正常停机信号。
        case QueryWorkerStepStatus::ReplyQueueRemoved:  // Reply 被删除时发送端也应停止，不能继续访问失效队列。
            return 0;  // 主进程关闭 IPC 是正常停止路径；Worker 不负责 remove()，避免多进程重复删除。
        case QueryWorkerStepStatus::RequestReceiveFailed:
        case QueryWorkerStepStatus::ReplySendFailed:
            return 1;  // 不可恢复 IPC 错误让 Supervisor 按异常退出路径重启 Worker，避免静默丢失后续任务。
        }
    }
}

MockQueryExecutionResult QueryWorkerRuntime::echo_mock_handler(const ipc::IpcTaskMsg& task, void*) {
    MockQueryExecutionResult result;
    result.status = ipc::IpcResponseStatus::Ok;
    result.payload = task.payload;  // Mock 阶段把请求正文原样回显，验证登录/查询数据的端到端二进制传输，不执行真实认证。
    return result;
}

bool QueryWorkerRuntime::options_are_valid() const noexcept {
    return options_.request_message_type > 0 &&
           options_.reply_message_type > 0 &&
           options_.protocol_limits.max_frame_bytes >= ipc::IpcWireLayout::kHeaderBytes &&
           options_.protocol_limits.max_frame_bytes <= std::numeric_limits<std::uint32_t>::max() &&
           receive_buffer_.size() == options_.protocol_limits.max_frame_bytes;
}

std::uint64_t QueryWorkerRuntime::now_unix_ms() const noexcept {
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return milliseconds <= 0 ? 0U : static_cast<std::uint64_t>(milliseconds);  // deadline 为非零无符号 wire 字段，系统异常时返回 0，任务仍会由主进程本地 deadline 保护。
}

void QueryWorkerRuntime::log_debug(const std::string& message) const {
    if (logger_ != nullptr) {
        logger_->debug(message);
    }
}

void QueryWorkerRuntime::log_warning(const std::string& message) const {
    if (logger_ != nullptr) {
        logger_->warn(message);
    }
}

void QueryWorkerRuntime::log_error(const std::string& message) const {
    if (logger_ != nullptr) {
        logger_->error(message);
    }
}

int query_worker_runtime_entry(void* context) {
    if (context == nullptr) {
        return 1;  // Supervisor 配置错误不能默默进入空循环，返回非零触发已有异常重启/告警路径。
    }
    return static_cast<QueryWorkerRuntime*>(context)->run();  // 仅在 fork 后的子进程调用；运行时对象和队列所有权仍保留在父进程。
}

}  // namespace oxmbs::worker
