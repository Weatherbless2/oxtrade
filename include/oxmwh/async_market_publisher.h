#pragma once

#include "oxmwh/market_publisher.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace oxmwh {

enum class MarketQueueFullPolicy {
    DropNewest,
    CoalesceSameSymbol,
};

struct AsyncMarketPublisherOptions {
    std::size_t capacity{4096};
    MarketQueueFullPolicy full_policy{MarketQueueFullPolicy::CoalesceSameSymbol};
};

struct AsyncMarketPublisherStats {
    std::uint64_t accepted{0};
    std::uint64_t published{0};
    std::uint64_t coalesced{0};
    std::uint64_t dropped{0};
    std::uint64_t publish_failed{0};
    std::size_t queue_depth{0};
    std::size_t queue_high_watermark{0};
};

// 把行情源回调和网络/IPC 发布解耦；它不改变 OxMwhService 的订阅与序号语义。
class AsyncMarketPublisher final : public IMarketPublisher {
public:
    AsyncMarketPublisher(IMarketPublisher& downstream,
                          AsyncMarketPublisherOptions options = {});
    ~AsyncMarketPublisher() override;

    bool start(std::string& error);
    void stop() noexcept;

    bool publish(const MarketEvent& event, std::string& error) override;
    bool running() const noexcept;
    AsyncMarketPublisherStats stats() const noexcept;

private:
    void run();

    IMarketPublisher& downstream_;
    AsyncMarketPublisherOptions options_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<MarketEvent> queue_;
    bool accepting_{false};
    bool thread_running_{false};
    std::thread worker_;
    AsyncMarketPublisherStats stats_;
};

}  // namespace oxmwh
