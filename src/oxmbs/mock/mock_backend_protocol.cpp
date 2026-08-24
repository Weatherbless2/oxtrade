#include "oxmbs/mock/mock_backend_protocol.h"

namespace oxmbs::mock {
namespace {

std::uint32_t message_type_for_service(std::string_view service) noexcept {
    if (service == "system" || service == "dictionaries" || service == "dictionary_details" ||
        service == "system_logs") {
        return kMockSystemMessageType;
    }
    if (service == "users" || service == "roles" || service == "sites" || service == "role_rights") {
        return kMockRightMessageType;
    }
    if (service == "funds" || service == "projects" || service == "accounts" || service == "fund_project_links") {
        return kMockFundMessageType;
    }
    if (service == "risk" || service == "risk_parameters" || service == "risk_events") {
        return kMockRiskMessageType;
    }
    return 0;
}

std::uint32_t function_id_for_operation(std::string_view operation) noexcept {
    if (operation == "page") {
        return kMockPageFunctionId;
    }
    if (operation == "heartbeat") {
        return kMockHeartbeatFunctionId;
    }
    if (operation == "login") {
        return kMockLoginFunctionId;
    }
    if (operation == "create") {
        return kMockCreateFunctionId;
    }
    if (operation == "update") {
        return kMockUpdateFunctionId;
    }
    if (operation == "transition") {
        return kMockTransitionFunctionId;
    }
    return 0;
}

}  // namespace

MockRoute resolve_mock_route(std::string_view service, std::string_view operation) noexcept {
    return {message_type_for_service(service), function_id_for_operation(operation)};  // Mock 路由只接受显式服务/操作组合，未知值不会猜测默认 LBM。
}

bool is_mock_message_type(std::uint32_t message_type) noexcept {
    return message_type == kMockSystemMessageType || message_type == kMockRightMessageType ||
           message_type == kMockFundMessageType || message_type == kMockRiskMessageType;
}

}  // namespace oxmbs::mock
