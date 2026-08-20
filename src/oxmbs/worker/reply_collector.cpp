#include "oxmbs/worker/reply_collector.h"

#include "oxmbs/logging/run_logger.h"

#include <exception>
#include <limits>

namespace oxmbs::worker {
namespace {

std::string response_context(const ipc::IpcRespMsg& response) {
    return "task_seq=" + std::to_string(response.task_seq) +
           " session_id=" + std::to_string(response.session_id) +
           " msg_type=" + std::to_string(response.msg_type) +
           " func_id=" + std::to_string(response.func_id);
}

}  // namespace

const char* reply_collector_step_status_name(ReplyCollectorStepStatus status) noexcept {
    switch (status) {
    case ReplyCollectorStepStatus::DeliveredToSink:
        return "DeliveredToSink";
    case ReplyCollectorStepStatus::ResponseSinkFailed:
        return "ResponseSinkFailed";
    case ReplyCollectorStepStatus::InvalidResponseDropped:
        return "InvalidResponseDropped";
    case ReplyCollectorStepStatus::CorrelationRejected:
        return "CorrelationRejected";
    case ReplyCollectorStepStatus::ReplyInterrupted:
        return "ReplyInterrupted";
    case ReplyCollectorStepStatus::ReplyQueueRemoved:
        return "ReplyQueueRemoved";
    case ReplyCollectorStepStatus::ReplyReceiveFailed:
        return "ReplyReceiveFailed";
    }
    return "Unknown";
}

ReplyCollector::ReplyCollector(ipc::SystemVMessageQueue& reply_queue,
                               ipc::TaskCorrelationRegistry& correlation_registry,
                               ReplyCollectorOptions options,
                               ResponseSink response_sink,
                               void* response_sink_context,
                               TimeoutSink timeout_sink,
                               void* timeout_sink_context,
                               logging::RunLogger* logger)
    : reply_queue_(reply_queue),
      correlation_registry_(correlation_registry),
      options_(options),
      response_sink_(response_sink),
      response_sink_context_(response_sink_context),
      timeout_sink_(timeout_sink),
      timeout_sink_context_(timeout_sink_context),
      logger_(logger),
      receive_buffer_(options.protocol_limits.max_frame_bytes) {
}

ReplyCollectorStepResult ReplyCollector::collect_one() {
    ReplyCollectorStepResult result;
    if (!options_are_valid()) {
        result.status = ReplyCollectorStepStatus::ReplyReceiveFailed;
        result.ipc_result = ipc::IpcResult::InvalidArgument;
        result.error = "ReplyCollector options are invalid";
        log_error(result.error);
        return result;
    }

    const auto received = reply_queue_.receive(
        options_.reply_message_type, receive_buffer_.data(), receive_buffer_.size(), false);  // 主进程可阻塞等待低频 Reply；网络 WorkFunc 不在这条路径上等待。
    result.ipc_result = received.result;
    if (!received) {
        result.error = received.error;
        switch (received.result) {
        case ipc::IpcResult::Interrupted:
            result.status = ReplyCollectorStepStatus::ReplyInterrupted;
            log_debug("reply collector receive interrupted");
            return result;
        case ipc::IpcResult::QueueRemoved:
            result.status = ReplyCollectorStepStatus::ReplyQueueRemoved;
            log_debug("reply collector queue removed; stopping without deleting IPC");
            return result;
        default:
            result.status = ReplyCollectorStepStatus::ReplyReceiveFailed;
            log_error("reply collector receive failed ipc_result=" +
                      std::string(ipc::ipc_result_name(received.result)) + " error=" + received.error);
            return result;
        }
    }

    const auto decoded = ipc::decode_response(receive_buffer_.data(), received.size, options_.protocol_limits);
    if (!decoded) {
        result.status = ReplyCollectorStepStatus::InvalidResponseDropped;
        result.error = decoded.error;
        log_warning("reply collector dropped invalid response protocol_status=" +
                    std::string(ipc::task_protocol_status_name(decoded.status)) + " error=" + decoded.error);  // 坏 Reply 已从队列取走，但不会污染 pending_ 或客户端会话。
        return result;
    }

    result.task_seq = decoded.message.task_seq;
    const auto correlation = correlation_registry_.resolve_response(decoded.message);
    result.correlation_status = correlation.status;
    if (!correlation) {
        result.status = ReplyCollectorStepStatus::CorrelationRejected;
        log_warning("reply collector rejected response correlation_status=" +
                    std::string(ipc::task_correlation_status_name(correlation.status)) + " " + response_context(decoded.message));
        return result;
    }

    if (response_sink_ != nullptr) {
        try {
            response_sink_(decoded.message, response_sink_context_);  // 只有三重关联匹配后才触碰主进程的 Mock/未来 HP-Socket 回包对象。
        } catch (const std::exception& exception) {
            result.status = ReplyCollectorStepStatus::ResponseSinkFailed;
            result.error = exception.what();
            log_error("reply collector response sink failed " + response_context(decoded.message) + " error=" + exception.what());
            return result;  // 关联记录已经安全消费，不能重新入队或重复回调；Collector 下一轮仍可处理其他 Reply。
        } catch (...) {
            result.status = ReplyCollectorStepStatus::ResponseSinkFailed;
            result.error = "response sink threw an unknown exception";
            log_error("reply collector response sink failed with unknown exception " + response_context(decoded.message));
            return result;
        }
    }
    result.status = ReplyCollectorStepStatus::DeliveredToSink;
    log_debug("reply collector delivered response " + response_context(decoded.message));
    return result;
}

std::vector<ipc::ExpiredTask> ReplyCollector::expire_pending() {
    auto expired = correlation_registry_.expire();  // 使用 #5 steady_clock deadline 清理主进程本地等待表，不依赖 Worker 的墙上时间判断。
    if (timeout_sink_ != nullptr) {
        for (const auto& task : expired) {
            try {
                timeout_sink_(task, timeout_sink_context_);  // 预留给未来 HP-Socket 超时包；#7 Mock 只交付精确 task/session 快照。
            } catch (const std::exception& exception) {
                log_error("reply collector timeout sink failed task_seq=" + std::to_string(task.task_seq) +
                          " session_id=" + std::to_string(task.session_id) + " error=" + exception.what());  // 一个超时通知失败不能阻止后续 pending 清理。
            } catch (...) {
                log_error("reply collector timeout sink failed with unknown exception task_seq=" +
                          std::to_string(task.task_seq) + " session_id=" + std::to_string(task.session_id));
            }
        }
    }
    return expired;
}

int ReplyCollector::run() {
    while (true) {
        const auto result = collect_one();
        switch (result.status) {
        case ReplyCollectorStepStatus::DeliveredToSink:
        case ReplyCollectorStepStatus::ResponseSinkFailed:
        case ReplyCollectorStepStatus::InvalidResponseDropped:
        case ReplyCollectorStepStatus::CorrelationRejected:
        case ReplyCollectorStepStatus::ReplyInterrupted:
            continue;
        case ReplyCollectorStepStatus::ReplyQueueRemoved:
            return 0;
        case ReplyCollectorStepStatus::ReplyReceiveFailed:
            return 1;
        }
    }
}

bool ReplyCollector::options_are_valid() const noexcept {
    return options_.reply_message_type > 0 &&
           options_.protocol_limits.max_frame_bytes >= ipc::IpcWireLayout::kHeaderBytes &&
           options_.protocol_limits.max_frame_bytes <= std::numeric_limits<std::uint32_t>::max() &&
           receive_buffer_.size() == options_.protocol_limits.max_frame_bytes;
}

void ReplyCollector::log_debug(const std::string& message) const {
    if (logger_ != nullptr) {
        logger_->debug(message);
    }
}

void ReplyCollector::log_warning(const std::string& message) const {
    if (logger_ != nullptr) {
        logger_->warn(message);
    }
}

void ReplyCollector::log_error(const std::string& message) const {
    if (logger_ != nullptr) {
        logger_->error(message);
    }
}

}  // namespace oxmbs::worker
