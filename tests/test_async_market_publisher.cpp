#include "oxmwh/async_market_publisher.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

class GatePublisher final : public oxmwh::IMarketPublisher {
public:
    bool publish(const oxmwh::MarketEvent& event, std::string&) override {
        std::unique_lock lock(mutex);
        events.push_back(event);
        entered.notify_all();
        if (events.size() == 1) {
            released.wait(lock, [&] { return release_first; });
        }
        return true;
    }

    void wait_until_first() {
        std::unique_lock lock(mutex);
        assert(entered.wait_for(lock, std::chrono::seconds(1), [&] { return events.size() >= 1; }));
    }

    void release() {
        {
            std::lock_guard lock(mutex);
            release_first = true;
        }
        released.notify_all();
    }

    std::mutex mutex;
    std::condition_variable entered;
    std::condition_variable released;
    std::vector<oxmwh::MarketEvent> events;
    bool release_first{false};
};

oxmwh::MarketEvent event(const char* symbol, double price) {
    oxmwh::MarketEvent value;
    value.symbol = symbol;
    value.exchange = "SSE";
    value.source_time = std::chrono::system_clock::now();
    value.last_price = price;
    return value;
}

}  // namespace

int main() {
    GatePublisher downstream;
    oxmwh::AsyncMarketPublisherOptions options;
    options.capacity = 1;
    oxmwh::AsyncMarketPublisher publisher(downstream, options);
    std::string error;
    assert(publisher.start(error));
    assert(publisher.publish(event("600519.SH", 1.0), error));
    downstream.wait_until_first();
    assert(publisher.publish(event("600519.SH", 2.0), error));
    assert(publisher.publish(event("600519.SH", 3.0), error));  // 同标的满队列时只替换尚未发布的旧快照。
    assert(!publisher.publish(event("000001.SZ", 4.0), error));
    assert(publisher.stats().coalesced == 1);
    assert(publisher.stats().dropped == 1);
    downstream.release();
    publisher.stop();
    assert(!publisher.running());
    assert(downstream.events.size() == 2);
    assert(downstream.events.back().last_price == 3.0);
    assert(publisher.stats().published == 2);
    return 0;
}
