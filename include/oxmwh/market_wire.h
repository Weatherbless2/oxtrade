#pragma once

#include "oxmwh/market_event.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oxmwh::wire {

// 固定头长度和字段上限属于协议契约；不能直接把 C++ 结构体内存当作网络报文。
constexpr std::uint32_t kMagic = 0x4F584D57;  // "OXMW"。
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kHeaderSize = 96;
constexpr std::size_t kMaxFrameSize = 1024 * 1024;
constexpr std::size_t kMaxBufferedBytes = 4 * kMaxFrameSize;
constexpr std::size_t kMaxInstrumentLength = 31;
constexpr std::size_t kMaxExchangeLength = 7;

enum class MessageType : std::uint16_t {
    Quote = 1,
    Heartbeat = 2,
    Snapshot = 3,
};

struct MarketWireMessage {
    MessageType message_type{MessageType::Quote};
    std::uint16_t flags{0};
    std::uint64_t request_id{0};
    std::uint64_t session_id{0};
    std::uint64_t source_sequence{0};
    std::uint64_t event_time_ns{0};
    std::string instrument_id;
    std::string exchange;
    std::vector<std::uint8_t> payload;
};

enum class DecodeStatus {
    Ok,
    Incomplete,
    Invalid,
    TooLarge,
};

struct DecodeResult {
    DecodeStatus status{DecodeStatus::Invalid};
    MarketWireMessage message;
    std::size_t consumed_bytes{0};
    std::vector<std::uint8_t> frame;
    std::string error;

    explicit operator bool() const noexcept { return status == DecodeStatus::Ok; }
};

// MarketWire 使用显式 little-endian 字段，保证 x86/Linux 与未来 Windows 适配不依赖 ABI。
bool encode(const MarketWireMessage& message,
            std::vector<std::uint8_t>& output,
            std::string& error);

DecodeResult decode(std::string_view bytes);

// 处理 TCP/HP-Socket 常见的半包和粘包；next() 每次只弹出一帧。
class StreamDecoder {
public:
    bool append(std::string_view bytes, std::string& error);
    DecodeResult next();
    std::size_t buffered_bytes() const noexcept { return buffer_.size(); }
    void clear() noexcept { buffer_.clear(); }

private:
    std::vector<std::uint8_t> buffer_;
};

// 把 #14 的标准行情事件转换为 MarketWire Quote，载荷同样显式编码。
bool encode_market_event(const MarketEvent& event,
                         MarketWireMessage& output,
                         std::string& error);

bool decode_market_event(const MarketWireMessage& message,
                         MarketEvent& output,
                         std::string& error);

}  // namespace oxmwh::wire
