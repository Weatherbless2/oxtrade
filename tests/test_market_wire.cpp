#include "oxmwh/market_wire.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace {

oxmwh::MarketEvent quote(std::uint64_t sequence) {
    oxmwh::MarketEvent event;
    event.symbol = "600519.SSE";
    event.exchange = "SSE";
    event.source_time = std::chrono::system_clock::now();
    event.source_sequence = sequence;
    event.last_price = 1450.25;
    event.volume = 1000;
    event.turnover = 1450250.0;
    for (std::size_t index = 0; index < 5; ++index) {
        event.bids[index] = {1450.0 - static_cast<double>(index), 100 + index};
        event.asks[index] = {1450.5 + static_cast<double>(index), 200 + index};
    }
    return event;
}

}  // namespace

int main() {
    oxmwh::wire::MarketWireMessage message;
    std::string error;
    assert(oxmwh::wire::encode_market_event(quote(42), message, error));

    std::vector<std::uint8_t> encoded;
    assert(oxmwh::wire::encode(message, encoded, error));
    assert(encoded.size() == oxmwh::wire::kHeaderSize + message.payload.size());

    const auto incomplete = oxmwh::wire::decode(std::string_view(
        reinterpret_cast<const char*>(encoded.data()), encoded.size() - 1));
    assert(incomplete.status == oxmwh::wire::DecodeStatus::Incomplete);

    oxmwh::wire::StreamDecoder decoder;
    assert(decoder.append(std::string_view(reinterpret_cast<const char*>(encoded.data()), 7), error));
    assert(decoder.next().status == oxmwh::wire::DecodeStatus::Incomplete);
    assert(decoder.append(std::string_view(reinterpret_cast<const char*>(encoded.data() + 7), encoded.size() - 7), error));
    const auto first = decoder.next();
    assert(first);
    oxmwh::MarketEvent decoded_event;
    assert(oxmwh::wire::decode_market_event(first.message, decoded_event, error));
    assert(decoded_event.symbol == "600519.SH");
    assert(decoded_event.source_sequence == 42);

    std::vector<std::uint8_t> sticky = encoded;
    sticky.insert(sticky.end(), encoded.begin(), encoded.end());
    oxmwh::wire::StreamDecoder sticky_decoder;
    assert(sticky_decoder.append(std::string_view(reinterpret_cast<const char*>(sticky.data()), sticky.size()), error));
    assert(sticky_decoder.next());
    assert(sticky_decoder.next());
    assert(sticky_decoder.next().status == oxmwh::wire::DecodeStatus::Incomplete);

    auto bad = encoded;
    bad[0] ^= 0x01;
    assert(oxmwh::wire::decode(std::string_view(reinterpret_cast<const char*>(bad.data()), bad.size())).status ==
           oxmwh::wire::DecodeStatus::Invalid);
    auto oversized = encoded;
    oversized[8] = 0xff;
    oversized[9] = 0xff;
    oversized[10] = 0xff;
    oversized[11] = 0x7f;
    assert(oxmwh::wire::decode(std::string_view(reinterpret_cast<const char*>(oversized.data()), oversized.size())).status ==
           oxmwh::wire::DecodeStatus::TooLarge);

    return 0;
}
