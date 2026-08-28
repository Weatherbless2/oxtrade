#include "oxmwh/market_event.h"

#include <cctype>
#include <cmath>

namespace oxmwh {
namespace {

std::string upper_copy(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(character))));
    }
    return result;
}

bool valid_exchange(std::string_view exchange) {
    return exchange == "SH" || exchange == "SZ" || exchange == "CFX" || exchange == "SSE" || exchange == "SZSE";
}

bool valid_level(const DepthLevel& level) {
    return std::isfinite(level.price) && level.price >= 0.0;
}

}  // namespace

std::string normalize_symbol(std::string_view symbol) {
    const std::string upper = upper_copy(symbol);
    const std::size_t separator = upper.find('.');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= upper.size() ||
        upper.find('.', separator + 1) != std::string::npos) {
        return {};
    }

    std::string ticker = upper.substr(0, separator);
    std::string exchange = upper.substr(separator + 1);
    for (const char character : ticker) {
        if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_' && character != '-') {
            return {};
        }
    }
    if (exchange == "SSE") {
        exchange = "SH";
    } else if (exchange == "SZSE") {
        exchange = "SZ";
    }
    if (!valid_exchange(exchange)) {
        return {};
    }
    return ticker + "." + exchange;
}

MarketEventValidation validate_event(const MarketEvent& event) {
    if (normalize_symbol(event.symbol).empty()) {
        return {false, "symbol is empty or has an unsupported exchange"};
    }
    const std::string exchange = upper_copy(event.exchange);
    if (!valid_exchange(exchange)) {
        return {false, "exchange is unsupported"};
    }
    if (event.source_time == std::chrono::system_clock::time_point{}) {
        return {false, "source_time is required"};
    }
    if (!std::isfinite(event.last_price) || event.last_price <= 0.0) {
        return {false, "last_price must be finite and positive"};
    }
    if (!std::isfinite(event.turnover) || event.turnover < 0.0) {
        return {false, "turnover must be finite and non-negative"};
    }
    for (const auto& level : event.bids) {
        if (!valid_level(level)) {
            return {false, "bid depth contains an invalid price"};
        }
    }
    for (const auto& level : event.asks) {
        if (!valid_level(level)) {
            return {false, "ask depth contains an invalid price"};
        }
    }
    return {true, {}};
}

}  // namespace oxmwh
