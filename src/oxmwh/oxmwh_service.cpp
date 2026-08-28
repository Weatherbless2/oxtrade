#include "oxmwh/oxmwh_service.h"

#include <algorithm>
#include <utility>

namespace oxmwh {

OxMwhService::OxMwhService(IMarketSource& source,
                           IMarketPublisher& publisher,
                           OxMwhServiceOptions options)
    : source_(source), publisher_(publisher), options_(options) {
}

OxMwhService::~OxMwhService() {
    stop();
}

bool OxMwhService::start(std::string& error) {
    {
        std::lock_guard lock(mutex_);
        if (running_) {
            return true;
        }
        if (options_.max_subscriptions == 0) {
            error = "max_subscriptions must be positive";
            return false;
        }
        running_ = true;  // 先设置生命周期状态，允许同步 Mock 源在 start() 内立即回调而不被误丢弃。
        source_status_ = MarketSourceStatus::Connecting;
    }

    if (!source_.start(
            [this](const MarketEvent& event) { on_source_event(event); },
            [this](MarketSourceStatus status) { on_source_status(status); },
            error)) {
        std::lock_guard lock(mutex_);
        running_ = false;
        source_status_ = MarketSourceStatus::Failed;
        return false;
    }
    return true;
}

void OxMwhService::stop() noexcept {
    bool should_stop = false;
    {
        std::lock_guard lock(mutex_);
        should_stop = running_;
        running_ = false;  // 先禁止新事件进入，再同步停止源，保证 stop 后不再发布行情。
        source_status_ = MarketSourceStatus::Stopped;
    }
    if (should_stop) {
        source_.stop();  // Source 的 stop 契约必须等待其回调线程退出，避免 this 被析构后仍被访问。
    }
}

bool OxMwhService::subscribe(const std::vector<std::string>& symbols, std::string& error) {
    std::vector<std::string> normalized;
    normalized.reserve(symbols.size());
    for (const std::string& symbol : symbols) {
        const std::string value = normalize_symbol(symbol);
        if (value.empty()) {
            error = "subscription contains an invalid symbol: " + symbol;
            return false;
        }
        if (std::find(normalized.begin(), normalized.end(), value) == normalized.end()) {
            normalized.push_back(value);  // 同一批次先去重，避免把重复订阅交给外部源。
        }
    }

    {
        std::lock_guard lock(mutex_);
        std::size_t new_count = subscriptions_.size();
        for (const auto& symbol : normalized) {
            if (subscriptions_.find(symbol) == subscriptions_.end()) {
                ++new_count;
            }
        }
        if (new_count > options_.max_subscriptions) {
            error = "subscription limit exceeded";
            return false;
        }
    }

    if (!source_.subscribe(normalized, error)) {
        return false;
    }
    std::lock_guard lock(mutex_);
    subscriptions_.insert(normalized.begin(), normalized.end());
    return true;
}

bool OxMwhService::unsubscribe(const std::vector<std::string>& symbols, std::string& error) {
    std::vector<std::string> normalized;
    normalized.reserve(symbols.size());
    for (const std::string& symbol : symbols) {
        const std::string value = normalize_symbol(symbol);
        if (value.empty()) {
            error = "unsubscription contains an invalid symbol: " + symbol;
            return false;
        }
        if (std::find(normalized.begin(), normalized.end(), value) == normalized.end()) {
            normalized.push_back(value);
        }
    }
    if (!source_.unsubscribe(normalized, error)) {
        return false;
    }
    std::lock_guard lock(mutex_);
    for (const auto& symbol : normalized) {
        subscriptions_.erase(symbol);
        last_sequences_.erase(symbol);  // 取消后重新订阅允许新源从 1 开始，不把旧会话序号误判成乱序。
    }
    return true;
}

bool OxMwhService::running() const noexcept {
    std::lock_guard lock(mutex_);
    return running_;
}

MarketSourceStatus OxMwhService::source_status() const noexcept {
    std::lock_guard lock(mutex_);
    return source_status_;
}

OxMwhServiceStats OxMwhService::stats() const noexcept {
    std::lock_guard lock(mutex_);
    return stats_;
}

void OxMwhService::on_source_event(const MarketEvent& event) {
    const MarketEventValidation validation = validate_event(event);
    if (!validation) {
        std::lock_guard lock(mutex_);
        ++stats_.invalid;
        return;  // 非法行情只计数丢弃，不能让坏数据进入发布端或下游 K 线聚合。
    }

    const std::string symbol = normalize_symbol(event.symbol);
    MarketEvent normalized_event = event;
    normalized_event.symbol = symbol;  // 发布端只接收统一代码，避免页面/缓存再重复做市场代码转换。
    {
        std::lock_guard lock(mutex_);
        ++stats_.received;
        if (!running_ || (options_.require_subscription && subscriptions_.find(symbol) == subscriptions_.end())) {
            ++stats_.unsubscribed;
            return;  // 未订阅的事件不向客户端扩散，避免全市场源把桌面客户端推送队列打满。
        }
        if (event.source_sequence != 0) {
            const auto previous = last_sequences_.find(symbol);
            if (previous != last_sequences_.end() && event.source_sequence <= previous->second) {
                ++stats_.duplicate_or_out_of_order;
                return;  // 同标的源序号不前进时丢弃，防止旧盘口覆盖新盘口。
            }
            last_sequences_[symbol] = event.source_sequence;
        }
    }

    std::string error;
    if (publisher_.publish(normalized_event, error)) {
        std::lock_guard lock(mutex_);
        ++stats_.published;
    } else {
        std::lock_guard lock(mutex_);
        ++stats_.publish_failed;
    }
}

void OxMwhService::on_source_status(MarketSourceStatus status) {
    std::lock_guard lock(mutex_);
    source_status_ = status;
}

}  // namespace oxmwh
