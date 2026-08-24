#pragma once

#include <cstdint>
#include <string_view>

namespace oxmbs::mock {

// 以下编号只服务于本地 MockBackend 联调；它们不是 Mentor 确认的 PackMessage 或正式消息枚举。
constexpr std::uint32_t kMockSystemMessageType{900001};
constexpr std::uint32_t kMockRightMessageType{900002};
constexpr std::uint32_t kMockFundMessageType{900003};
constexpr std::uint32_t kMockRiskMessageType{900004};
constexpr std::uint32_t kMockMarketMessageType{900005};
constexpr std::uint32_t kMockTradeMessageType{900006};
constexpr std::uint32_t kMockQueryMessageType{900007};

constexpr std::uint32_t kMockPageFunctionId{1};
constexpr std::uint32_t kMockHeartbeatFunctionId{2};
constexpr std::uint32_t kMockLoginFunctionId{3};
constexpr std::uint32_t kMockCreateFunctionId{4};
constexpr std::uint32_t kMockUpdateFunctionId{5};
constexpr std::uint32_t kMockTransitionFunctionId{6};
constexpr std::uint32_t kMockQuotesFunctionId{7};
constexpr std::uint32_t kMockTicksFunctionId{8};
constexpr std::uint32_t kMockBarsFunctionId{9};
constexpr std::uint32_t kMockDepthFunctionId{10};
constexpr std::uint32_t kMockSubscribeFunctionId{11};
constexpr std::uint32_t kMockPlaceOrderFunctionId{12};
constexpr std::uint32_t kMockCancelOrderFunctionId{13};
constexpr std::uint32_t kMockFillOrderFunctionId{14};
constexpr std::uint32_t kMockLogoutFunctionId{15};

struct MockRoute {
    std::uint32_t message_type{0};
    std::uint32_t function_id{0};

    explicit operator bool() const noexcept { return message_type != 0 && function_id != 0; }
};

MockRoute resolve_mock_route(std::string_view service, std::string_view operation) noexcept;
bool is_mock_message_type(std::uint32_t message_type) noexcept;

}  // namespace oxmbs::mock
