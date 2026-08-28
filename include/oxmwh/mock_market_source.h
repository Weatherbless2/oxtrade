#pragma once

#include "oxmwh/market_source.h"

#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace oxmwh::mock {

// 同仓库的确定性 Mock 源，只用于单测和页面联调，不代表正式券商接入协议。
class MockMarketSource final : public IMarketSource {
public:
    bool start(MarketEventCallback event_callback,
               MarketStatusCallback status_callback,
               std::string& error) override;
    void stop() noexcept override;
    bool subscribe(const std::vector<std::string>& symbols, std::string& error) override;
    bool unsubscribe(const std::vector<std::string>& symbols, std::string& error) override;

    void emit(const MarketEvent& event);
    bool started() const noexcept;

private:
    mutable std::mutex mutex_;
    bool started_{false};
    MarketEventCallback event_callback_;
    MarketStatusCallback status_callback_;
    std::unordered_set<std::string> subscriptions_;
};

}  // namespace oxmwh::mock
