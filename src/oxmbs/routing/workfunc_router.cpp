#include "oxmbs/routing/workfunc_router.h"

#include "oxmbs/logging/run_logger.h"

#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace oxmbs::routing {
namespace {

std::string task_context(const MockPackMessage& message, ipc::TaskSequence task_seq = 0) {
    std::string context = "session_id=" + std::to_string(message.session_id) +
                          " msg_type=" + std::to_string(message.msg_type) +
                          " func_id=" + std::to_string(message.func_id);
    if (task_seq != 0) {
        context += " task_seq=" + std::to_string(task_seq);
    }
    return context;
}

}  // namespace

const char* workfunc_route_status_name(WorkFuncRouteStatus status) noexcept {
    switch (status) {
    case WorkFuncRouteStatus::HighFrequencyPassthrough:
        return "HighFrequencyPassthrough";
    case WorkFuncRouteStatus::ForwardedToWorker:
        return "ForwardedToWorker";
    case WorkFuncRouteStatus::InvalidMessage:
        return "InvalidMessage";
    case WorkFuncRouteStatus::InvalidRouterOptions:
        return "InvalidRouterOptions";
    case WorkFuncRouteStatus::PayloadTooLarge:
        return "PayloadTooLarge";
    case WorkFuncRouteStatus::PayloadCopyFailed:
        return "PayloadCopyFailed";
    case WorkFuncRouteStatus::RegistryRejected:
        return "RegistryRejected";
    case WorkFuncRouteStatus::EncodingRejected:
        return "EncodingRejected";
    case WorkFuncRouteStatus::QueueSendFailed:
        return "QueueSendFailed";
    }
    return "Unknown";
}

WorkFuncRouter::WorkFuncRouter(ipc::IpcQueueManager& queue_manager,
                               ipc::TaskCorrelationRegistry& correlation_registry,
                               MessageRoutePolicy route_policy,
                               WorkFuncRouterOptions options,
                               logging::RunLogger* logger)
    : queue_manager_(queue_manager),
      correlation_registry_(correlation_registry),
      route_policy_(std::move(route_policy)),
      options_(options),
      logger_(logger) {
}

bool WorkFuncRouter::is_query_or_manage_msg(const MockPackMessage& message) const noexcept {
    return route_policy_.is_query_or_manage_msg(message.msg_type);  // 白名单未命中一律保留在原高频 WorkFunc 分支。
}

WorkFuncRouteResult WorkFuncRouter::route(const MockPackMessage& message) {
    // msg_type=0 无法被可靠分类，必须快速拒绝；高频消息则到此返回，payload 不校验、不复制、不入 IPC。
    if (message.msg_type == 0) {
        WorkFuncRouteResult result;
        result.status = WorkFuncRouteStatus::InvalidMessage;
        result.error = "message type must be non-zero";
        log_warning("workfunc route drop reason=invalid-message " + task_context(message));
        return result;
    }
    if (!is_query_or_manage_msg(message)) {
        WorkFuncRouteResult result;
        result.status = WorkFuncRouteStatus::HighFrequencyPassthrough;
        return result;
    }
    return forward_to_worker_process_pool(message);
}

WorkFuncRouteResult WorkFuncRouter::forward_to_worker_process_pool(const MockPackMessage& message) {
    WorkFuncRouteResult result;
    // 即使未来调用点误绕过 route()，这里仍重复执行白名单闸门，保证交易/行情等高频消息不能直接被送进 IPC。
    if (message.msg_type == 0) {
        result.status = WorkFuncRouteStatus::InvalidMessage;
        result.error = "message type must be non-zero";
        log_warning("workfunc route drop reason=invalid-message " + task_context(message));
        return result;
    }
    if (!is_query_or_manage_msg(message)) {
        result.status = WorkFuncRouteStatus::HighFrequencyPassthrough;
        return result;
    }
    if (!options_are_valid()) {
        result.status = WorkFuncRouteStatus::InvalidRouterOptions;
        result.error = "router timeout, request mtype or protocol limits are invalid";
        log_warning("workfunc route drop reason=invalid-options " + task_context(message));
        return result;
    }
    if (message.msg_type == 0 || message.session_id == 0 || message.data == nullptr || message.data_len == 0) {
        result.status = WorkFuncRouteStatus::InvalidMessage;
        result.error = "low-frequency message requires non-zero type/session and non-empty payload";
        log_warning("workfunc route drop reason=invalid-message " + task_context(message));
        return result;
    }

    const auto maximum_payload = options_.protocol_limits.max_frame_bytes - ipc::IpcWireLayout::kHeaderBytes;
    if (message.data_len > maximum_payload) {  // 先做常数时间长度判断，避免为注定无法进队的报文分配或复制内存。
        result.status = WorkFuncRouteStatus::PayloadTooLarge;
        result.error = "payload exceeds configured IPC task frame limit";
        log_warning("workfunc route drop reason=payload-too-large " + task_context(message));
        return result;
    }

    ipc::IpcPayload payload;
    try {
        payload.assign(message.data, message.data + message.data_len);  // 关键深拷贝：上游无锁队列释放/复用原 buffer 后，Worker 仍只使用自己的稳定字节副本。
    } catch (const std::bad_alloc&) {
        result.status = WorkFuncRouteStatus::PayloadCopyFailed;
        result.error = "cannot allocate payload copy";
        log_warning("workfunc route drop reason=payload-copy-failed " + task_context(message));
        return result;
    } catch (const std::length_error&) {
        result.status = WorkFuncRouteStatus::PayloadCopyFailed;
        result.error = "payload copy exceeds vector capacity";
        log_warning("workfunc route drop reason=payload-copy-too-large " + task_context(message));  // 容器容量边界同样必须在登记前拒绝，不能留下 pending_。
        return result;
    }

    const auto registration = correlation_registry_.register_request(message.session_id, options_.query_task_timeout);
    result.task_seq = registration.task_seq;
    result.correlation_status = registration.status;
    if (!registration) {
        result.status = WorkFuncRouteStatus::RegistryRejected;
        result.error = "cannot register IPC task: " + std::string(ipc::task_correlation_status_name(registration.status));
        log_warning("workfunc route drop reason=registry-rejected " + task_context(message));
        return result;
    }

    ipc::IpcTaskMsg task;
    task.task_seq = registration.task_seq;
    task.session_id = registration.session_id;
    task.deadline_unix_ms = registration.deadline_unix_ms;
    task.msg_type = message.msg_type;
    task.func_id = message.func_id;
    task.payload = std::move(payload);  // 注册后只移动已完成的私有副本，后续绝不再读取上游 message.data。

    ipc::IpcPayload frame;
    std::string error;
    const auto encoding_result = ipc::encode_task(task, frame, error, options_.protocol_limits);
    if (encoding_result != ipc::TaskProtocolStatus::Ok) {
        result.status = WorkFuncRouteStatus::EncodingRejected;
        result.error = "cannot encode IPC task: " + error;
        result.rollback_status = correlation_registry_.cancel_request(
            registration.task_seq, registration.session_id, registration.deadline_unix_ms);  // 编码失败意味着 Task 从未可见，必须撤销刚登记的 pending_。
        log_warning("workfunc route drop reason=encode-failed status=" +
                    std::string(ipc::task_protocol_status_name(encoding_result)) + " " + task_context(message, registration.task_seq));
        return result;
    }

    const auto send_result = queue_manager_.request_queue().send(
        options_.request_message_type, frame.data(), frame.size(), true, error);  // true -> IPC_NOWAIT；WorkFunc 无论队列是否已满都不能卡住。
    result.ipc_result = send_result;
    if (send_result != ipc::IpcResult::Ok) {
        result.status = WorkFuncRouteStatus::QueueSendFailed;
        result.error = "cannot enqueue IPC task: " + error;
        result.rollback_status = correlation_registry_.cancel_request(
            registration.task_seq, registration.session_id, registration.deadline_unix_ms);  // 入队失败时消除 pending_，防止主进程稍后报告不存在的超时请求。
        log_warning("workfunc route drop reason=queue-send-failed ipc_result=" +
                    std::string(ipc::ipc_result_name(send_result)) + " " + task_context(message, registration.task_seq));
        return result;
    }

    result.status = WorkFuncRouteStatus::ForwardedToWorker;
    log_debug("workfunc route forwarded request-queue-mtype=" + std::to_string(options_.request_message_type) + " " +
              task_context(message, registration.task_seq));  // 成功日志只针对低频请求，不在高频直通路径增加额外写盘开销。
    return result;
}

bool WorkFuncRouter::options_are_valid() const noexcept {
    return options_.query_task_timeout > std::chrono::milliseconds::zero() &&
           options_.request_message_type > 0 &&
           options_.protocol_limits.max_frame_bytes > ipc::IpcWireLayout::kHeaderBytes &&  // 路由要求非空 payload，因此固定头外至少还要有 1 字节正文空间。
           options_.protocol_limits.max_frame_bytes <= std::numeric_limits<std::uint32_t>::max();
}

void WorkFuncRouter::log_debug(const std::string& message) const {
    if (logger_ != nullptr) {
        logger_->debug(message);
    }
}

void WorkFuncRouter::log_warning(const std::string& message) const {
    if (logger_ != nullptr) {
        logger_->warn(message);
    }
}

}  // namespace oxmbs::routing
