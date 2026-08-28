#pragma once

#include "oxmwh/market_publisher.h"
#include "oxmwh/market_source.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace oxmwh {

struct OxMwhServiceOptions {
    std::size_t max_subscriptions{4096};
    bool require_subscription{true};
};

struct OxMwhServiceStats {
    std::uint64_t received{0};
    std::uint64_t published{0};
    std::uint64_t invalid{0};
    std::uint64_t unsubscribed{0};
    std::uint64_t duplicate_or_out_of_order{0};
    std::uint64_t publish_failed{0};
};

// oxMWH 的核心编排：行情源 -> 标准化校验 -> 订阅/乱序闸门 -> 发布适配器。
// 它只做内存级快速处理，不在行情回调线程执行数据库、阻塞网络或交易逻辑。
class OxMwhService {
public:
    OxMwhService(IMarketSource& source,
                 IMarketPublisher& publisher,
                 OxMwhServiceOptions options = {});
    ~OxMwhService();

    bool start(std::string& error);
    void stop() noexcept;

    bool subscribe(const std::vector<std::string>& symbols, std::string& error);
    bool unsubscribe(const std::vector<std::string>& symbols, std::string& error);

    bool running() const noexcept;
    MarketSourceStatus source_status() const noexcept;
    OxMwhServiceStats stats() const noexcept;

private:
    void on_source_event(const MarketEvent& event);
    void on_source_status(MarketSourceStatus status);

    IMarketSource& source_;
    IMarketPublisher& publisher_;
    OxMwhServiceOptions options_;
    mutable std::mutex mutex_;
    bool running_{false};
    MarketSourceStatus source_status_{MarketSourceStatus::Stopped};
    std::unordered_set<std::string> subscriptions_;
    std::unordered_map<std::string, std::uint64_t> last_sequences_;
    OxMwhServiceStats stats_;
};

}  // namespace oxmwh
