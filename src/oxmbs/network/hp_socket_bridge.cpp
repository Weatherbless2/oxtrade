#include "oxmbs/network/hp_socket_bridge.h"

#include "oxmbs/logging/run_logger.h"

#include <chrono>

namespace oxmbs::network {
namespace {

std::uint64_t now_unix_ms() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string json_error(const std::string& message) {
    std::string escaped;
    escaped.reserve(message.size());
    for (const char character : message) {
        if (character == '\\' || character == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return "{\"schema_version\":1,\"error\":\"" + escaped + "\"}";
}

}  // namespace

HpSocketBridge::HpSocketBridge(HpSocketServer* server,
                               routing::WorkFuncRouter& router,
                               logging::RunLogger* logger) noexcept
    : server_(server), router_(&router), logger_(logger) {
}

void HpSocketBridge::attach_server(HpSocketServer& server) noexcept {
    server_ = &server;
}

void HpSocketBridge::on_frame(ConnectionId connection_id,
                              const PackMessage& message,
                              void* context) {
    if (context != nullptr) {
        static_cast<HpSocketBridge*>(context)->handle_frame(connection_id, message);
    }
}

void HpSocketBridge::on_connection_closed(ConnectionId connection_id,
                                           std::uint64_t session_id,
                                           void* context) {
    if (context != nullptr) {
        auto* bridge = static_cast<HpSocketBridge*>(context);
        bridge->log_info("network connection closed connection_id=" + std::to_string(connection_id) +
                         " session_id=" + std::to_string(session_id));
    }
}

void HpSocketBridge::on_response(const ipc::IpcRespMsg& response, void* context) {
    if (context != nullptr) {
        static_cast<HpSocketBridge*>(context)->handle_response(response);
    }
}

void HpSocketBridge::on_timeout(const ipc::ExpiredTask& task, void* context) {
    if (context != nullptr) {
        static_cast<HpSocketBridge*>(context)->handle_timeout(task);
    }
}

void HpSocketBridge::handle_frame(ConnectionId connection_id, const PackMessage& message) {
    if (server_ == nullptr || router_ == nullptr) {
        return;
    }
    if (message.kind != PackMessageKind::Request) {
        send_error(message, message.session_id, PackMessageErrorCode::InvalidHeader,
                   "only request PackMessage frames may enter the business router");
        return;
    }
    if (message.session_id == 0) {
        send_error(message, 0, PackMessageErrorCode::NotAuthenticated,
                   "session_id is required for QMT requests");
        return;
    }

    std::string error;
    if (!server_->bind_session(connection_id, message.session_id, error)) {
        send_error(message, message.session_id, PackMessageErrorCode::SessionMismatch, error);
        return;
    }

    const auto routed = router_->route(message);  // WorkFuncRouter 在此完成白名单、深拷贝、关联登记和 IPC_NOWAIT 投递。
    if (routed.forwarded()) {
        return;
    }
    send_error(message, message.session_id, route_error(routed.status),
               routed.error.empty() ? routing::workfunc_route_status_name(routed.status) : routed.error);
}

void HpSocketBridge::handle_response(const ipc::IpcRespMsg& response) {
    if (server_ == nullptr) {
        return;
    }
    auto message = make_response(response);
    std::string error;
    if (!server_->send_to_session(message.session_id, message, error)) {
        log_warning("cannot send IPC response to session_id=" + std::to_string(message.session_id) +
                    " request_id=" + std::to_string(message.request_id) + " error=" + error);
    }
}

void HpSocketBridge::handle_timeout(const ipc::ExpiredTask& task) {
    if (server_ == nullptr || task.session_id == 0) {
        return;
    }
    auto message = make_timeout_response(task);
    std::string error;
    if (!server_->send_to_session(message.session_id, message, error)) {
        log_warning("cannot send timeout response to session_id=" + std::to_string(message.session_id) +
                    " request_id=" + std::to_string(message.request_id) + " error=" + error);
    }
}

void HpSocketBridge::send_error(const PackMessage& request,
                                std::uint64_t session_id,
                                PackMessageErrorCode code,
                                const std::string& message) {
    if (server_ == nullptr) {
        return;
    }
    PackMessage response;
    response.kind = PackMessageKind::Response;
    response.request_id = request.request_id;
    response.session_id = session_id;
    response.message_type = request.message_type;
    response.function_id = request.function_id;
    response.timestamp_ms = now_unix_ms();
    response.error_code = static_cast<std::uint32_t>(code);
    const auto body = json_error(message);
    response.payload.assign(body.begin(), body.end());
    std::string error;
    if (!server_->send_to_session(session_id, response, error)) {
        log_warning("cannot send immediate error response session_id=" + std::to_string(session_id) +
                    " request_id=" + std::to_string(request.request_id) + " error=" + error);
    }
}

PackMessageErrorCode HpSocketBridge::route_error(routing::WorkFuncRouteStatus status) noexcept {
    switch (status) {
    case routing::WorkFuncRouteStatus::PayloadTooLarge:
    case routing::WorkFuncRouteStatus::InvalidMessage:
    case routing::WorkFuncRouteStatus::InvalidRouterOptions:
        return PackMessageErrorCode::InvalidArgument;
    case routing::WorkFuncRouteStatus::QueueSendFailed:
        return PackMessageErrorCode::RateLimited;
    case routing::WorkFuncRouteStatus::HighFrequencyPassthrough:
        return PackMessageErrorCode::FunctionNotAvailable;
    case routing::WorkFuncRouteStatus::PayloadCopyFailed:
    case routing::WorkFuncRouteStatus::RegistryRejected:
    case routing::WorkFuncRouteStatus::EncodingRejected:
        return PackMessageErrorCode::InternalError;
    case routing::WorkFuncRouteStatus::ForwardedToWorker:
        return PackMessageErrorCode::Ok;
    }
    return PackMessageErrorCode::InternalError;
}

PackMessage HpSocketBridge::make_response(const ipc::IpcRespMsg& response) {
    PackMessage message;
    message.kind = PackMessageKind::Response;
    message.request_id = response.request_id != 0 ? response.request_id : response.task_seq;
    message.session_id = response.session_id;
    message.message_type = static_cast<std::uint16_t>(response.msg_type);
    message.function_id = static_cast<std::uint16_t>(response.func_id);
    message.sequence = response.task_seq;
    message.timestamp_ms = now_unix_ms();
    message.error_code = response.status == ipc::IpcResponseStatus::Ok
                             ? static_cast<std::uint32_t>(PackMessageErrorCode::Ok)
                             : response.status == ipc::IpcResponseStatus::Rejected
                                   ? static_cast<std::uint32_t>(PackMessageErrorCode::InvalidArgument)
                                   : response.status == ipc::IpcResponseStatus::TimedOut
                                         ? static_cast<std::uint32_t>(PackMessageErrorCode::RequestTimeout)
                                         : static_cast<std::uint32_t>(PackMessageErrorCode::InternalError);
    message.payload = response.payload;
    return message;
}

PackMessage HpSocketBridge::make_timeout_response(const ipc::ExpiredTask& task) {
    PackMessage message;
    message.kind = PackMessageKind::Response;
    message.request_id = task.request_id != 0 ? task.request_id : task.task_seq;
    message.session_id = task.session_id;
    message.message_type = static_cast<std::uint16_t>(task.msg_type);
    message.function_id = static_cast<std::uint16_t>(task.func_id);
    message.sequence = task.task_seq;
    message.timestamp_ms = now_unix_ms();
    message.error_code = static_cast<std::uint32_t>(PackMessageErrorCode::RequestTimeout);
    const auto body = json_error("query request timed out before a Reply was collected");
    message.payload.assign(body.begin(), body.end());
    return message;
}

void HpSocketBridge::log_warning(const std::string& message) const {
    if (logger_ != nullptr) {
        logger_->warn(message);
    }
}

void HpSocketBridge::log_info(const std::string& message) const {
    if (logger_ != nullptr) {
        logger_->info(message);
    }
}

}  // namespace oxmbs::network
