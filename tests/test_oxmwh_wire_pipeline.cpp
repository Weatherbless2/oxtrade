#include "oxmwh/async_market_publisher.h"
#include "oxmwh/market_wire.h"
#include "oxmwh/mock_market_source.h"
#include "oxmwh/oxmwh_service.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace {

class WireSink final : public oxmwh::wire::IMarketWireSink {
public:
    bool send(const std::vector<std::uint8_t>& frame, std::string&) override {
        frames.push_back(frame);
        return true;
    }

    std::vector<std::vector<std::uint8_t>> frames;
};

oxmwh::MarketEvent quote() {
    oxmwh::MarketEvent event;
    event.symbol = "600519.SSE";
    event.exchange = "SSE";
    event.source_time = std::chrono::system_clock::now();
    event.source_sequence = 9;
    event.last_price = 1450.5;
    event.volume = 900;
    event.turnover = 1305450.0;
    event.bids[0] = {1450.4, 100};
    event.asks[0] = {1450.6, 120};
    return event;
}

}  // namespace

int main() {
    WireSink sink;
    oxmwh::wire::MarketWirePublisher wire_publisher(sink);
    oxmwh::AsyncMarketPublisher async_publisher(wire_publisher);
    oxmwh::mock::MockMarketSource source;
    oxmwh::OxMwhService service(source, async_publisher);
    std::string error;

    assert(async_publisher.start(error));
    assert(service.start(error));
    assert(service.subscribe({"600519.SH"}, error));
    source.emit(quote());
    service.stop();
    async_publisher.stop();

    assert(sink.frames.size() == 1);
    const auto decoded = oxmwh::wire::decode(std::string_view(
        reinterpret_cast<const char*>(sink.frames.front().data()), sink.frames.front().size()));
    assert(decoded);
    oxmwh::MarketEvent restored;
    assert(oxmwh::wire::decode_market_event(decoded.message, restored, error));
    assert(restored.symbol == "600519.SH");
    assert(restored.last_price == 1450.5);
    assert(restored.source_sequence == 9);
    return 0;
}
