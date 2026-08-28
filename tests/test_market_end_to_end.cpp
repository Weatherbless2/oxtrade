#include "oxmbs/market/market_wire_broadcaster.h"
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

class ClientSink final : public oxmbs::market::IMarketClientSink {
public:
    bool send(const std::string& client_id,
              const std::vector<std::uint8_t>& frame,
              std::string&) override {
        last_client = client_id;
        frames.push_back(frame);
        return true;
    }

    std::string last_client;
    std::vector<std::vector<std::uint8_t>> frames;
};

class BroadcasterSink final : public oxmwh::wire::IMarketWireSink {
public:
    explicit BroadcasterSink(oxmbs::market::MarketWireBroadcaster& broadcaster)
        : broadcaster_(broadcaster) {}

    bool send(const std::vector<std::uint8_t>& frame, std::string& error) override {
        return broadcaster_.on_source_bytes(
            "oxMWH-mock", std::string_view(reinterpret_cast<const char*>(frame.data()), frame.size()), error);
    }

private:
    oxmbs::market::MarketWireBroadcaster& broadcaster_;
};

oxmwh::MarketEvent quote() {
    oxmwh::MarketEvent event;
    event.symbol = "000001.SZ";
    event.exchange = "SZSE";
    event.source_time = std::chrono::system_clock::now();
    event.source_sequence = 1;
    event.last_price = 12.34;
    event.volume = 500;
    event.turnover = 6170.0;
    event.bids[0] = {12.33, 100};
    event.asks[0] = {12.35, 120};
    return event;
}

}  // namespace

int main() {
    ClientSink client_sink;
    oxmbs::market::MarketWireBroadcaster broadcaster(client_sink);
    std::string error;
    assert(broadcaster.subscribe("oxApp-session-1", {"000001.SZ"}, error));
    BroadcasterSink wire_sink(broadcaster);
    oxmwh::wire::MarketWirePublisher wire_publisher(wire_sink);
    oxmwh::AsyncMarketPublisher async_publisher(wire_publisher);
    oxmwh::mock::MockMarketSource source;
    oxmwh::OxMwhService service(source, async_publisher);

    assert(async_publisher.start(error));
    assert(service.start(error));
    assert(service.subscribe({"000001.SZ"}, error));
    source.emit(quote());
    service.stop();
    async_publisher.stop();

    assert(client_sink.last_client == "oxApp-session-1");
    assert(client_sink.frames.size() == 1);
    const auto decoded = oxmwh::wire::decode(std::string_view(
        reinterpret_cast<const char*>(client_sink.frames.front().data()), client_sink.frames.front().size()));
    assert(decoded);
    assert(decoded.message.instrument_id == "000001.SZ");
    assert(broadcaster.stats().decoded_frames == 1);
    assert(broadcaster.stats().delivered_frames == 1);
    return 0;
}
