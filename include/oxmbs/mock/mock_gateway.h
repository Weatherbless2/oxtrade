#pragma once

#include <map>
#include <string>
#include <vector>

namespace oxmbs::mock {

using MockForm = std::map<std::string, std::string>;

struct MockGatewayResult {
    std::string app_status{"succeeded"};
    std::string error_code{"none"};
    std::string error_message;
    std::string payload_json{"{}"};
    bool success{true};
};

// Mock-only 的行情/交易服务，模拟架构图中的 oxMWH、交易网关和查询核心。
// 它不连接真实券商、交易所、HP-Socket 或数据库，正式接入时由对应服务替换。
class MockGateway {
public:
    MockGateway();

    MockGatewayResult handle(const std::string& service,
                             const std::string& operation,
                             const MockForm& form);

private:
    MockGatewayResult page(const std::string& resource, const MockForm& form) const;
    MockGatewayResult subscribe(const MockForm& form) const;
    MockGatewayResult place_order(const MockForm& form);
    MockGatewayResult cancel_order(const MockForm& form);
    MockGatewayResult fill_order(const MockForm& form);

    void refresh_account_totals(const std::string& account_id);

    std::map<std::string, std::vector<std::map<std::string, std::string>>> resources_;
    int next_order_id_{1};
    int next_trade_id_{1};
};

}  // namespace oxmbs::mock
