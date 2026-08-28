#pragma once

#include "oxmwh/market_event.h"

#include <string>

namespace oxmwh {

// 发布端可以是 oxMBS/HP-Socket 适配器、进程内回调或测试接收器；正式协议由适配器独立实现。
class IMarketPublisher {
public:
    virtual ~IMarketPublisher() = default;

    virtual bool publish(const MarketEvent& event, std::string& error) = 0;
};

}  // namespace oxmwh
