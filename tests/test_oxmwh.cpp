#include "oxmwh/market_event.h"
#include "oxmwh/mock_market_source.h"
#include "oxmwh/oxmwh_service.h"

#include <cassert>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace {

class CollectingPublisher final : public oxmwh::IMarketPublisher {
public:
    bool publish(const oxmwh::MarketEvent& event, std::string&) override {
        events.push_back(event);
        return !fail;
    }

    std::vector<oxmwh::MarketEvent> events;
    bool fail{false};
};

oxmwh::MarketEvent event(std::string symbol, std::uint64_t sequence, double price = 10.0) {
    oxmwh::MarketEvent result;
    result.symbol = std::move(symbol);
    result.exchange = "SSE";
    result.source_time = std::chrono::system_clock::now();
    result.source_sequence = sequence;
    result.last_price = price;
    result.volume = 100;
    result.turnover = 1000.0;
    result.bids[0] = {price - 0.01, 100};
    result.asks[0] = {price + 0.01, 100};
    return result;
}

}  // namespace

int main() {
    assert(oxmwh::normalize_symbol("600519.SSE") == "600519.SH");
    assert(oxmwh::normalize_symbol("IF2609.CFX") == "IF2609.CFX");
    assert(oxmwh::normalize_symbol("bad symbol.SH").empty());

    oxmwh::mock::MockMarketSource source;
    CollectingPublisher publisher;
    oxmwh::OxMwhService service(source, publisher);
    std::string error;

    assert(service.start(error));
    assert(service.running());
    assert(service.source_status() == oxmwh::MarketSourceStatus::Running);
    assert(service.subscribe({"600519.SSE"}, error));

    source.emit(event("600519.SH", 1, 1450.0));
    source.emit(event("600519.SH", 1, 1451.0));  // 重复源序号必须丢弃，不能覆盖已发布的最新盘口。
    source.emit(event("600519.SH", 0, 1452.0));  // 无序号的源事件允许通过，兼容尚未提供序号的开发 Mock。
    source.emit(event("000001.SZ", 1, 12.0));  // 未订阅标的不应扩散到发布端。

    assert(publisher.events.size() == 2);
    assert(publisher.events.front().symbol == "600519.SH");
    const auto stats = service.stats();
    assert(stats.received == 4);
    assert(stats.published == 2);
    assert(stats.duplicate_or_out_of_order == 1);
    assert(stats.unsubscribed == 1);

    auto invalid = event("600519.SH", 3, -1.0);
    source.emit(invalid);
    assert(service.stats().invalid == 1);

    publisher.fail = true;
    source.emit(event("600519.SH", 4, 1453.0));
    assert(service.stats().publish_failed == 1);

    assert(service.unsubscribe({"600519.SH"}, error));
    service.stop();
    assert(!service.running());
    assert(!source.started());
    return 0;
}
