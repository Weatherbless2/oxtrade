#pragma once

#include <cstdint>
#include <string_view>

namespace oxmbs::mock {

// 以下编号只服务于本地 MockBackend 联调；它们不是 Mentor 确认的 PackMessage 或正式消息枚举。
constexpr std::uint32_t kMockSystemMessageType{900001};
constexpr std::uint32_t kMockRightMessageType{900002};
constexpr std::uint32_t kMockFundMessageType{900003};
constexpr std::uint32_t kMockRiskMessageType{900004};

constexpr std::uint32_t kMockPageFunctionId{1};
constexpr std::uint32_t kMockHeartbeatFunctionId{2};
constexpr std::uint32_t kMockLoginFunctionId{3};
constexpr std::uint32_t kMockCreateFunctionId{4};
constexpr std::uint32_t kMockUpdateFunctionId{5};
constexpr std::uint32_t kMockTransitionFunctionId{6};

struct MockRoute {
    std::uint32_t message_type{0};
    std::uint32_t function_id{0};

    explicit operator bool() const noexcept { return message_type != 0 && function_id != 0; }
};

MockRoute resolve_mock_route(std::string_view service, std::string_view operation) noexcept;
bool is_mock_message_type(std::uint32_t message_type) noexcept;

}  // namespace oxmbs::mock
