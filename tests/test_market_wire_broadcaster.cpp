#include "oxmbs/market/market_wire_broadcaster.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

class CollectingSink final : public oxmbs::market::IMarketClientSink {
public:
    bool send(const std::string& client_id,
              const std::vector<std::uint8_t>& frame,
              std::string&) override {
        deliveries.emplace_back(client_id, frame);
        return !fail;
    }

    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> deliveries;
    bool fail{false};
};

std::vector<std::uint8_t> encoded_quote(std::uint64_t sequence, const char* symbol) {
    oxmwh::MarketEvent event;
    event.symbol = symbol;
    event.exchange = "SSE";
    event.source_time = std::chrono::system_clock::now();
    event.source_sequence = sequence;
    event.last_price = 10.0 + static_cast<double>(sequence);
    event.volume = 100;
    event.turnover = 1000.0;
    event.bids[0] = {event.last_price - 0.01, 100};
    event.asks[0] = {event.last_price + 0.01, 100};
    oxmwh::wire::MarketWireMessage message;
    std::string error;
    assert(oxmwh::wire::encode_market_event(event, message, error));
    std::vector<std::uint8_t> frame;
    assert(oxmwh::wire::encode(message, frame, error));
    return frame;
}

}  // namespace

int main() {
    CollectingSink sink;
    oxmbs::market::MarketWireBroadcaster broadcaster(sink);
    std::string error;
    assert(broadcaster.subscribe("client-a", {"600519.SH"}, error));
    assert(broadcaster.subscribe("client-b", {"000001.SZ"}, error));

    const auto first = encoded_quote(1, "600519.SSE");
    assert(broadcaster.on_source_bytes("mock-source", std::string_view(
        reinterpret_cast<const char*>(first.data()), 5), error));
    assert(sink.deliveries.empty());
    assert(broadcaster.on_source_bytes("mock-source", std::string_view(
        reinterpret_cast<const char*>(first.data() + 5), first.size() - 5), error));
    assert(sink.deliveries.size() == 1);
    assert(sink.deliveries.front().first == "client-a");

    assert(broadcaster.on_source_bytes("mock-source", std::string_view(
        reinterpret_cast<const char*>(first.data()), first.size()), error));
    assert(sink.deliveries.size() == 1);  // 重复源序号被源端闸门丢弃。

    const auto gap = encoded_quote(3, "600519.SH");
    std::string gap_symbol;
    std::uint64_t expected = 0;
    std::uint64_t actual = 0;
    broadcaster.set_snapshot_recovery_callback([&](const std::string& symbol, std::uint64_t e, std::uint64_t a) {
        gap_symbol = symbol;
        expected = e;
        actual = a;
    });
    assert(broadcaster.on_source_bytes("mock-source", std::string_view(
        reinterpret_cast<const char*>(gap.data()), gap.size()), error));
    assert(sink.deliveries.size() == 2);
    assert(gap_symbol == "600519.SH");
    assert(expected == 2 && actual == 3);

    const auto filtered = encoded_quote(1, "000001.SZ");
    assert(broadcaster.on_source_bytes("mock-source", std::string_view(
        reinterpret_cast<const char*>(filtered.data()), filtered.size()), error));
    assert(sink.deliveries.size() == 3);
    assert(sink.deliveries.back().first == "client-b");

    broadcaster.remove_client("client-b");
    assert(broadcaster.stats().clients == 1);
    assert(broadcaster.stats().sequence_gaps == 1);
    assert(broadcaster.stats().duplicate_or_out_of_order == 1);

    return 0;
}
