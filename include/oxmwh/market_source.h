#pragma once

#include "oxmwh/market_event.h"

#include <functional>
#include <string>
#include <vector>

namespace oxmwh {

enum class MarketSourceStatus {
    Stopped,
    Connecting,
    Running,
    Reconnecting,
    Failed,
};

using MarketEventCallback = std::function<void(const MarketEvent&)>;
using MarketStatusCallback = std::function<void(MarketSourceStatus)>;

// 行情源只负责接入和产生事件；它不负责缓存、页面刷新或交易下单。
class IMarketSource {
public:
    virtual ~IMarketSource() = default;

    virtual bool start(MarketEventCallback event_callback,
                       MarketStatusCallback status_callback,
                       std::string& error) = 0;
    virtual void stop() noexcept = 0;
    virtual bool subscribe(const std::vector<std::string>& symbols, std::string& error) = 0;
    virtual bool unsubscribe(const std::vector<std::string>& symbols, std::string& error) = 0;
};

}  // namespace oxmwh
