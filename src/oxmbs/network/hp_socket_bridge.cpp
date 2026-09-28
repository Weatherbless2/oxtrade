#include "oxmbs/network/hp_socket_bridge.h"

#include "oxmbs/logging/run_logger.h"

#include <cctype>
#include <chrono>
#include <charconv>
#include <map>
#include <string_view>

#include <limits>

namespace oxmbs::network {
namespace {

std::uint64_t now_unix_ms() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string json_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        if (character == '\\' || character == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

std::string json_string(std::string_view value) {
    return "\"" + json_escape(value) + "\"";
}

std::string json_error(PackMessageErrorCode code, std::string_view message) {
    return "{\"schema_version\":1,\"data\":{},\"error_code\":" +
           std::to_string(static_cast<std::uint32_t>(code)) +
           ",\"message\":" + json_string(message) + ",\"warnings\":[]}";
}

std::uint64_t unix_ms(std::chrono::system_clock::time_point value) noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        value.time_since_epoch()).count());
}

// 认证请求只允许一个 JSON 对象和原子字段；递归下降解析器拒绝重复键、坏转义和尾随字节。
class AuthJsonObjectParser {
public:
    explicit AuthJsonObjectParser(std::string_view input) : input_(input) {}

    bool parse(std::map<std::string, std::string>& output, std::string& error) {
        skip_space();
        if (!consume('{')) {
            error = "auth payload must be a JSON object";
            return false;
        }
        skip_space();
        if (consume('}')) {
            skip_space();
            if (position_ != input_.size()) {
                error = "auth payload contains trailing bytes";
                return false;
            }
            return true;
        }
        while (true) {
            std::string key;
            if (!parse_string(key, error)) {
                return false;
            }
            skip_space();
            if (!consume(':')) {
                error = "auth payload is missing ':'";
                return false;
            }
            skip_space();
            std::string value;
            if (!parse_scalar(value, error)) {
                return false;
            }
            if (!output.emplace(std::move(key), std::move(value)).second) {
                error = "auth payload contains duplicate fields";
                return false;
            }
            skip_space();
            if (consume('}')) {
                skip_space();
                if (position_ != input_.size()) {
                    error = "auth payload contains trailing bytes";
                    return false;
                }
                return true;
            }
            if (!consume(',')) {
                error = "auth payload is missing ','";
                return false;
            }
            skip_space();
        }
    }

private:
    bool parse_scalar(std::string& output, std::string& error) {
        if (position_ < input_.size() && input_[position_] == '"') {
            return parse_string(output, error);
        }
        const auto begin = position_;
        while (position_ < input_.size() && input_[position_] != ',' && input_[position_] != '}' &&
               !std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
        if (begin == position_) {
            error = "auth payload contains an empty value";
            return false;
        }
        output = std::string(input_.substr(begin, position_ - begin));
        return true;
    }

    bool parse_string(std::string& output, std::string& error) {
        if (!consume('"')) {
            error = "auth payload field name/value must be a string";
            return false;
        }
        output.clear();
        while (position_ < input_.size()) {
            const char character = input_[position_++];
            if (character == '"') {
                return true;
            }
            if (character == '\\') {
                if (position_ >= input_.size()) {
                    error = "auth payload contains an incomplete escape";
                    return false;
                }
                const char escaped = input_[position_++];
                switch (escaped) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                default:
                    error = "auth payload contains an unsupported string escape";
                    return false;
                }
            } else if (static_cast<unsigned char>(character) < 0x20U) {
                error = "auth payload contains a control character";
                return false;
            } else {
                output.push_back(character);
            }
        }
        error = "auth payload contains an unterminated string";
        return false;
    }

    bool consume(char expected) {
        if (position_ >= input_.size() || input_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    void skip_space() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_])) != 0) {
            ++position_;
        }
    }

    std::string_view input_;
    std::size_t position_{0};
};

bool required_auth_string(const std::map<std::string, std::string>& fields,
                          std::string_view key,
                          std::string& output,
                          std::string& error) {
    const auto iterator = fields.find(std::string(key));
    if (iterator == fields.end() || iterator->second.empty()) {
        error = std::string(key) + " is required";
        return false;
    }
    output = iterator->second;
    return true;
}

bool valid_schema(const std::map<std::string, std::string>& fields, std::string& error) {
    const auto iterator = fields.find("schema_version");
    if (iterator == fields.end() || iterator->second != "1") {
        error = "schema_version must be 1";
        return false;
    }
    return true;
}

bool parse_auth_fields(const PackMessage& message,
                       std::map<std::string, std::string>& fields,
                       std::string& error) {
    const auto* bytes = message.payload.empty()
                            ? ""
                            : reinterpret_cast<const char*>(message.payload.data());
    AuthJsonObjectParser parser(std::string_view(bytes, message.payload.size()));
    return parser.parse(fields, error) && valid_schema(fields, error);
}

std::string auth_data_document(std::string_view data) {
    return "{\"schema_version\":1,\"data\":" + std::string(data) +
           ",\"error_code\":0,\"message\":\"\",\"warnings\":[]}";
}

std::string permissions_json(const std::vector<std::string>& permissions) {
    std::string result = "[";
    for (std::size_t index = 0; index < permissions.size(); ++index) {
        if (index != 0) {
            result.push_back(',');
        }
        result += json_string(permissions[index]);
    }
    result.push_back(']');
    return result;
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

void HpSocketBridge::attach_auth_service(auth::AuthService& service) noexcept {
    auth_service_ = &service;
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
        if (bridge->auth_service_ != nullptr && session_id != 0) {
            std::string error;
            bridge->auth_service_->disconnect(session_id, error);  // 连接关闭后立即撤销 Session，禁止旧 Token 跨连接复用。
        }
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
        send_error(connection_id, message, message.session_id, PackMessageErrorCode::InvalidHeader,
                   "only request PackMessage frames may enter the business router");
        return;
    }
    if (message.message_type == 1) {
        handle_auth_frame(connection_id, message);
        return;
    }
    if (auth_service_ == nullptr) {
        send_error(connection_id, message, message.session_id, PackMessageErrorCode::InternalError,
                   "authentication service is not configured");
        return;
    }
    if (message.session_id == 0) {
        send_error(connection_id, message, 0, PackMessageErrorCode::NotAuthenticated,
                   "session_id is required for QMT requests");
        return;
    }

    std::string error;
    if (!server_->bind_session(connection_id, message.session_id, error)) {
        send_error(connection_id, message, message.session_id, PackMessageErrorCode::SessionMismatch, error);
        return;
    }
    const auto permission = required_permission(message);
    if (permission == nullptr) {
        send_error(connection_id, message, message.session_id, PackMessageErrorCode::FunctionNotAvailable,
                   "business permission is not registered for this message");
        return;
    }
    const auto authorization = auth_service_->authorize_bound_session(message.session_id, permission, error);
    if (authorization != auth::AuthStatus::Ok) {
        send_error(connection_id, message, message.session_id, auth_error(authorization), error);
        return;
    }

    const auto routed = router_->route(message);  // WorkFuncRouter 在此完成白名单、深拷贝、关联登记和 IPC_NOWAIT 投递。
    if (routed.forwarded()) {
        return;
    }
    send_error(connection_id, message, message.session_id, route_error(routed.status),
               routed.error.empty() ? routing::workfunc_route_status_name(routed.status) : routed.error);
}

void HpSocketBridge::handle_auth_frame(ConnectionId connection_id, const PackMessage& message) {
    if (auth_service_ == nullptr) {
        send_error(connection_id, message, 0, PackMessageErrorCode::InternalError,
                   "authentication service is not configured");
        return;
    }
    if (message.function_id == 0) {
        std::map<std::string, std::string> fields;
        std::string error;
        auth::AuthChallengeRequest request;
        if (!parse_auth_fields(message, fields, error) ||
            !required_auth_string(fields, "username", request.username, error) ||
            !required_auth_string(fields, "client_nonce", request.client_nonce, error)) {
            send_auth_response(connection_id, message, 0, PackMessageErrorCode::InvalidArgument, "{}", error);
            return;
        }
        const auto result = auth_service_->issue_challenge(request);
        if (!result) {
            send_auth_response(connection_id, message, 0, auth_error(result.status), "{}", result.error);
            return;
        }
        const std::string data = "{\"challenge_id\":" + json_string(result.challenge_id) +
                                 ",\"server_nonce\":" + json_string(result.server_nonce) +
                                 ",\"expires_at_unix_ms\":" + std::to_string(unix_ms(result.expires_at)) + "}";
        send_auth_response(connection_id, message, 0, PackMessageErrorCode::Ok, data);
        return;
    }
    if (message.function_id == 1) {
        std::map<std::string, std::string> fields;
        std::string error;
        auth::AuthLoginRequest request;
        if (!parse_auth_fields(message, fields, error) ||
            !required_auth_string(fields, "username", request.username, error) ||
            !required_auth_string(fields, "challenge_id", request.challenge_id, error) ||
            !required_auth_string(fields, "client_nonce", request.client_nonce, error) ||
            !required_auth_string(fields, "password_proof", request.password_proof, error)) {
            send_auth_response(connection_id, message, 0, PackMessageErrorCode::InvalidArgument, "{}", error);
            return;
        }
        const auto result = auth_service_->login(request);
        if (!result) {
            send_auth_response(connection_id, message, 0, auth_error(result.status), "{}", result.error);
            return;
        }
        if (!server_->bind_session(connection_id, result.session_id, error)) {
            std::string ignored;
            auth_service_->disconnect(result.session_id, ignored);
            send_auth_response(connection_id, message, 0, PackMessageErrorCode::InternalError, "{}",
                               "login session could not be bound to the connection");
            return;
        }
        const std::string data = "{\"session_id\":" + std::to_string(result.session_id) +
                                 ",\"access_token\":" + json_string(result.access_token) +
                                 ",\"expires_at_unix_ms\":" + std::to_string(unix_ms(result.expires_at)) +
                                 ",\"permissions\":" + permissions_json(result.permissions) +
                                 ",\"user\":{\"user_name\":" + json_string(request.username) +
                                 ",\"display_name\":" + json_string(request.username) + "}}";
        send_auth_response(connection_id, message, result.session_id, PackMessageErrorCode::Ok, data);
        return;
    }
    if (message.function_id == 2 || message.function_id == 3) {
        if (message.session_id == 0) {
            send_auth_response(connection_id, message, 0, PackMessageErrorCode::NotAuthenticated, "{}",
                               "session_id is required");
            return;
        }
        std::string error;
        if (!server_->bind_session(connection_id, message.session_id, error)) {
            send_auth_response(connection_id, message, message.session_id, PackMessageErrorCode::SessionMismatch, "{}", error);
            return;
        }
        std::map<std::string, std::string> fields;
        if (!parse_auth_fields(message, fields, error)) {
            send_auth_response(connection_id, message, message.session_id, PackMessageErrorCode::InvalidArgument, "{}", error);
            return;
        }
        std::string access_token;
        if (!required_auth_string(fields, "access_token", access_token, error)) {
            send_auth_response(connection_id, message, message.session_id, PackMessageErrorCode::InvalidArgument, "{}", error);
            return;
        }
        if (message.function_id == 2) {
            const auto status = auth_service_->logout(message.session_id, access_token, error);
            if (status != auth::AuthStatus::Ok) {
                send_auth_response(connection_id, message, message.session_id, auth_error(status), "{}", error);
                return;
            }
            std::string ignored;
            server_->unbind_session(connection_id, message.session_id, ignored);
            send_auth_response(connection_id, message, message.session_id, PackMessageErrorCode::Ok, "{}");
            return;
        }
        const auto result = auth_service_->refresh(message.session_id, access_token);
        if (!result) {
            send_auth_response(connection_id, message, message.session_id, auth_error(result.status), "{}", result.error);
            return;
        }
        const std::string data = "{\"session_id\":" + std::to_string(result.session_id) +
                                 ",\"access_token\":" + json_string(result.access_token) +
                                 ",\"expires_at_unix_ms\":" + std::to_string(unix_ms(result.expires_at)) +
                                 ",\"permissions\":" + permissions_json(result.permissions) + "}";
        send_auth_response(connection_id, message, message.session_id, PackMessageErrorCode::Ok, data);
        return;
    }
    send_auth_response(connection_id, message, message.session_id, PackMessageErrorCode::FunctionNotAvailable,
                       "{}", "unsupported auth function");
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

void HpSocketBridge::send_error(ConnectionId connection_id,
                                const PackMessage& request,
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
    const auto body = json_error(code, message);
    response.payload.assign(body.begin(), body.end());
    std::string error;
    if (!server_->send(connection_id, response, error)) {
        log_warning("cannot send immediate error response session_id=" + std::to_string(session_id) +
                    " request_id=" + std::to_string(request.request_id) + " error=" + error);
    }
}

void HpSocketBridge::send_auth_response(ConnectionId connection_id,
                                        const PackMessage& request,
                                        std::uint64_t session_id,
                                        PackMessageErrorCode code,
                                        const std::string& data,
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
    const auto body = code == PackMessageErrorCode::Ok
                          ? auth_data_document(data)
                          : json_error(code, message);
    response.payload.assign(body.begin(), body.end());
    std::string error;
    if (!server_->send(connection_id, response, error)) {
        log_warning("cannot send auth response connection_id=" + std::to_string(connection_id) +
                    " request_id=" + std::to_string(request.request_id) + " error=" + error);
    }
}

PackMessageErrorCode HpSocketBridge::auth_error(auth::AuthStatus status) noexcept {
    switch (status) {
    case auth::AuthStatus::InvalidArgument: return PackMessageErrorCode::InvalidArgument;
    case auth::AuthStatus::ChallengeExpired:
    case auth::AuthStatus::ChallengeNotFound:
    case auth::AuthStatus::CredentialRejected:
    case auth::AuthStatus::SessionLimitReached: return PackMessageErrorCode::AuthFailed;
    case auth::AuthStatus::SessionExpired: return PackMessageErrorCode::SessionExpired;
    case auth::AuthStatus::SessionNotFound: return PackMessageErrorCode::NotAuthenticated;
    case auth::AuthStatus::PermissionDenied: return PackMessageErrorCode::PermissionDenied;
    case auth::AuthStatus::Ok: return PackMessageErrorCode::Ok;
    }
    return PackMessageErrorCode::InternalError;
}

const char* HpSocketBridge::required_permission(const PackMessage& request) noexcept {
    if (request.message_type != 2) {
        return nullptr;
    }
    switch (request.function_id) {
    case 1: return "market.instrument.read";
    case 2: return "market.calendar.read";
    case 3: return "market.quote.read";
    default: return nullptr;
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
    const auto body = json_error(PackMessageErrorCode::RequestTimeout,
                                 "query request timed out before a Reply was collected");
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
