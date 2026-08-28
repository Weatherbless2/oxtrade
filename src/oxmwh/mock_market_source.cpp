#include "oxmwh/mock_market_source.h"

#include "oxmwh/market_event.h"

#include <utility>

namespace oxmwh::mock {

bool MockMarketSource::start(MarketEventCallback event_callback,
                             MarketStatusCallback status_callback,
                             std::string& error) {
    if (!event_callback || !status_callback) {
        error = "MockMarketSource requires event and status callbacks";
        return false;
    }
    MarketStatusCallback running_callback;
    {
        std::lock_guard lock(mutex_);
        started_ = true;
        event_callback_ = std::move(event_callback);
        status_callback_ = std::move(status_callback);
        running_callback = status_callback_;
    }
    running_callback(MarketSourceStatus::Running);  // Mock 源同步进入 Running，测试无需依赖睡眠等待状态。
    return true;
}

void MockMarketSource::stop() noexcept {
    MarketStatusCallback status_callback;
    {
        std::lock_guard lock(mutex_);
        if (!started_) {
            return;
        }
        started_ = false;
        event_callback_ = {};
        status_callback = status_callback_;
        status_callback_ = {};
        subscriptions_.clear();
    }
    if (status_callback) {
        status_callback(MarketSourceStatus::Stopped);
    }
}

bool MockMarketSource::subscribe(const std::vector<std::string>& symbols, std::string& error) {
    std::lock_guard lock(mutex_);
    if (!started_) {
        error = "MockMarketSource is not started";
        return false;
    }
    for (const std::string& symbol : symbols) {
        const std::string normalized = normalize_symbol(symbol);
        if (normalized.empty()) {
            error = "invalid symbol: " + symbol;
            return false;
        }
        subscriptions_.insert(normalized);
    }
    return true;
}

bool MockMarketSource::unsubscribe(const std::vector<std::string>& symbols, std::string& error) {
    std::lock_guard lock(mutex_);
    if (!started_) {
        error = "MockMarketSource is not started";
        return false;
    }
    for (const std::string& symbol : symbols) {
        const std::string normalized = normalize_symbol(symbol);
        if (normalized.empty()) {
            error = "invalid symbol: " + symbol;
            return false;
        }
        subscriptions_.erase(normalized);
    }
    return true;
}

void MockMarketSource::emit(const MarketEvent& event) {
    MarketEventCallback callback;
    {
        std::lock_guard lock(mutex_);
        if (!started_) {
            return;
        }
        callback = event_callback_;
    }
    if (callback) {
        callback(event);  // 复制回调后锁外执行，避免发布端重入 Mock 源造成互斥锁死锁。
    }
}

bool MockMarketSource::started() const noexcept {
    std::lock_guard lock(mutex_);
    return started_;
}

}  // namespace oxmwh::mock
