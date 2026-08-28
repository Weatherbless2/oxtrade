#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace oxmwh {

struct DepthLevel {
    double price{0.0};
    std::uint64_t volume{0};
};

struct MarketEvent {
    std::string symbol;
    std::string exchange;
    std::chrono::system_clock::time_point source_time{};
    std::chrono::system_clock::time_point received_at{};
    std::uint64_t source_sequence{0};
    double last_price{0.0};
    std::uint64_t volume{0};
    double turnover{0.0};
    std::array<DepthLevel, 5> bids{};
    std::array<DepthLevel, 5> asks{};
};

struct MarketEventValidation {
    bool valid{false};
    std::string error;

    explicit operator bool() const noexcept { return valid; }
};

// 统一代码格式，避免不同行情源把同一标的写成 600519.SSE/600519.SH 后产生重复订阅。
std::string normalize_symbol(std::string_view symbol);

// 在进入缓存、推送和后续策略前做一次轻量边界校验，避免 NaN/负价格污染下游计算。
MarketEventValidation validate_event(const MarketEvent& event);

}  // namespace oxmwh
