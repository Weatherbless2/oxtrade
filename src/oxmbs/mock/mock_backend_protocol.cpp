#include "oxmbs/mock/mock_backend_protocol.h"

namespace oxmbs::mock {
namespace {

std::uint32_t message_type_for_service(std::string_view service) noexcept {
    if (service == "system" || service == "dictionaries" || service == "dictionary_details" ||
        service == "system_logs" || service == "service_status") {
        return kMockSystemMessageType;
    }
    if (service == "users" || service == "roles" || service == "sites" || service == "cololist" ||
        service == "role_rights" || service == "user_menu_rights" || service == "fund_rights" || service == "project_rights") {
        return kMockRightMessageType;
    }
    if (service == "funds" || service == "projects" || service == "accounts" || service == "fund_account_links") {
        return kMockFundMessageType;
    }
    if (service == "risk" || service == "risk_parameters" || service == "risk_events") {
        return kMockRiskMessageType;
    }
    if (service == "market") {
        return kMockMarketMessageType;
    }
    if (service == "trade") {
        return kMockTradeMessageType;
    }
    if (service == "query") {
        return kMockQueryMessageType;
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
    if (operation == "quotes") return kMockQuotesFunctionId;
    if (operation == "ticks") return kMockTicksFunctionId;
    if (operation == "bars") return kMockBarsFunctionId;
    if (operation == "depth") return kMockDepthFunctionId;
    if (operation == "subscribe") return kMockSubscribeFunctionId;
    if (operation == "place_order") return kMockPlaceOrderFunctionId;
    if (operation == "cancel_order") return kMockCancelOrderFunctionId;
    if (operation == "fill_order") return kMockFillOrderFunctionId;
    if (operation == "logout") return kMockLogoutFunctionId;
    return 0;
}

}  // namespace

MockRoute resolve_mock_route(std::string_view service, std::string_view operation) noexcept {
    return {message_type_for_service(service), function_id_for_operation(operation)};  // Mock 路由只接受显式服务/操作组合，未知值不会猜测默认 LBM。
}

bool is_mock_message_type(std::uint32_t message_type) noexcept {
    return message_type == kMockSystemMessageType || message_type == kMockRightMessageType ||
           message_type == kMockFundMessageType || message_type == kMockRiskMessageType ||
           message_type == kMockMarketMessageType || message_type == kMockTradeMessageType ||
           message_type == kMockQueryMessageType;
}

}  // namespace oxmbs::mock
