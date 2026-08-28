#include "oxmwh/market_wire.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace oxmwh::wire {
namespace {

constexpr std::size_t kFrameLengthOffset = 8;
constexpr std::size_t kPayloadLengthOffset = 48;
constexpr std::size_t kInstrumentLengthOffset = 52;
constexpr std::size_t kExchangeLengthOffset = 54;
constexpr std::size_t kInstrumentOffset = 60;
constexpr std::size_t kExchangeOffset = kInstrumentOffset + 32;
constexpr std::size_t kBasePayloadSize = 8 + 8 + 8 + (5 * 16) + (5 * 16);

void write_u16(std::vector<std::uint8_t>& output, std::size_t offset, std::uint16_t value) {
    output[offset] = static_cast<std::uint8_t>(value);
    output[offset + 1] = static_cast<std::uint8_t>(value >> 8);
}

void write_u32(std::vector<std::uint8_t>& output, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        output[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

void write_u64(std::vector<std::uint8_t>& output, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        output[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
    }
}

std::uint16_t read_u16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>(bytes[0]) |
           static_cast<std::uint16_t>(bytes[1]) << 8;
}

std::uint32_t read_u32(const std::uint8_t* bytes) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8);
    }
    return value;
}

std::uint64_t read_u64(const std::uint8_t* bytes) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8);
    }
    return value;
}

void write_double(std::vector<std::uint8_t>& output, std::size_t offset, double value) {
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));  // 只复制 IEEE754 位模式，不依赖结构体布局或对齐。
    write_u64(output, offset, bits);
}

double read_double(const std::uint8_t* bytes) {
    const std::uint64_t bits = read_u64(bytes);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool valid_fixed_text(std::string_view value, std::size_t max_length, std::string_view field, std::string& error) {
    if (value.empty() || value.size() > max_length) {
        error = std::string(field) + " length is outside protocol limit";
        return false;
    }
    if (value.find('\0') != std::string_view::npos) {
        error = std::string(field) + " must not contain NUL";
        return false;
    }
    return true;
}

std::uint64_t to_unix_ns(std::chrono::system_clock::time_point time) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count());
}

std::chrono::system_clock::time_point from_unix_ns(std::uint64_t value) {
    return std::chrono::system_clock::time_point(std::chrono::nanoseconds(value));
}

bool valid_event_payload(const MarketWireMessage& message, std::string& error) {
    if (message.message_type != MessageType::Quote) {
        error = "market event payload requires Quote message type";
        return false;
    }
    if (message.payload.size() != kBasePayloadSize) {
        error = "market event payload has an unexpected length";
        return false;
    }
    if (message.event_time_ns == 0) {
        error = "market event timestamp is required";
        return false;
    }
    return true;
}

}  // namespace

bool MarketWirePublisher::publish(const MarketEvent& event, std::string& error) {
    MarketWireMessage message;
    if (!encode_market_event(event, message, error)) {
        return false;
    }
    std::vector<std::uint8_t> frame;
    if (!encode(message, frame, error)) {
        return false;
    }
    return sink_.send(frame, error);  // 网络适配器只接收完整帧；重试和连接状态由它自己的生命周期管理。
}

bool encode(const MarketWireMessage& message,
            std::vector<std::uint8_t>& output,
            std::string& error) {
    if (!valid_fixed_text(message.instrument_id, kMaxInstrumentLength, "instrument_id", error) ||
        !valid_fixed_text(message.exchange, kMaxExchangeLength, "exchange", error)) {
        return false;
    }
    if (message.payload.size() > kMaxFrameSize - kHeaderSize) {
        error = "market wire payload is too large";
        return false;
    }
    const std::size_t frame_size = kHeaderSize + message.payload.size();
    output.assign(frame_size, 0);
    write_u32(output, 0, kMagic);
    write_u16(output, 4, kVersion);
    write_u16(output, 6, static_cast<std::uint16_t>(kHeaderSize));
    write_u32(output, kFrameLengthOffset, static_cast<std::uint32_t>(frame_size));
    write_u16(output, 12, static_cast<std::uint16_t>(message.message_type));
    write_u16(output, 14, message.flags);
    write_u64(output, 16, message.request_id);
    write_u64(output, 24, message.session_id);
    write_u64(output, 32, message.source_sequence);
    write_u64(output, 40, message.event_time_ns);
    write_u32(output, kPayloadLengthOffset, static_cast<std::uint32_t>(message.payload.size()));
    write_u16(output, kInstrumentLengthOffset, static_cast<std::uint16_t>(message.instrument_id.size()));
    write_u16(output, kExchangeLengthOffset, static_cast<std::uint16_t>(message.exchange.size()));
    std::copy(message.instrument_id.begin(), message.instrument_id.end(), output.begin() + kInstrumentOffset);
    std::copy(message.exchange.begin(), message.exchange.end(), output.begin() + kExchangeOffset);
    std::copy(message.payload.begin(), message.payload.end(), output.begin() + kHeaderSize);
    return true;
}

DecodeResult decode(std::string_view bytes) {
    DecodeResult result;
    if (bytes.size() < 12) {
        result.status = DecodeStatus::Incomplete;
        return result;
    }
    const auto* raw = reinterpret_cast<const std::uint8_t*>(bytes.data());
    if (read_u32(raw) != kMagic) {
        result.status = DecodeStatus::Invalid;
        result.error = "market wire magic mismatch";
        return result;
    }
    const std::uint16_t version = read_u16(raw + 4);
    const std::uint16_t header_len = read_u16(raw + 6);
    const std::uint32_t frame_len = read_u32(raw + kFrameLengthOffset);
    if (version != kVersion || header_len != kHeaderSize) {
        result.status = DecodeStatus::Invalid;
        result.error = "market wire version or header length mismatch";
        return result;
    }
    if (frame_len < kHeaderSize) {
        result.status = DecodeStatus::Invalid;
        result.error = "market wire frame length is smaller than header";
        return result;
    }
    if (frame_len > kMaxFrameSize) {
        result.status = DecodeStatus::TooLarge;
        result.error = "market wire frame exceeds maximum size";
        return result;
    }
    if (bytes.size() < frame_len) {
        result.status = DecodeStatus::Incomplete;
        return result;
    }
    const std::uint32_t payload_len = read_u32(raw + kPayloadLengthOffset);
    if (payload_len != frame_len - kHeaderSize) {
        result.status = DecodeStatus::Invalid;
        result.error = "market wire payload length does not match frame length";
        return result;
    }
    const std::uint16_t instrument_len = read_u16(raw + kInstrumentLengthOffset);
    const std::uint16_t exchange_len = read_u16(raw + kExchangeLengthOffset);
    if (instrument_len == 0 || instrument_len > kMaxInstrumentLength ||
        exchange_len == 0 || exchange_len > kMaxExchangeLength) {
        result.status = DecodeStatus::Invalid;
        result.error = "market wire text field length is outside limit";
        return result;
    }
    result.message.message_type = static_cast<MessageType>(read_u16(raw + 12));
    result.message.flags = read_u16(raw + 14);
    result.message.request_id = read_u64(raw + 16);
    result.message.session_id = read_u64(raw + 24);
    result.message.source_sequence = read_u64(raw + 32);
    result.message.event_time_ns = read_u64(raw + 40);
    result.message.instrument_id.assign(reinterpret_cast<const char*>(raw + kInstrumentOffset), instrument_len);
    result.message.exchange.assign(reinterpret_cast<const char*>(raw + kExchangeOffset), exchange_len);
    result.message.payload.assign(raw + kHeaderSize, raw + frame_len);
    result.consumed_bytes = frame_len;
    result.frame.assign(raw, raw + frame_len);  // 广播可以复用已验证的原始帧，不重复序列化改变字节。
    result.status = DecodeStatus::Ok;
    return result;
}

bool StreamDecoder::append(std::string_view bytes, std::string& error) {
    if (bytes.size() > kMaxBufferedBytes - buffer_.size()) {
        error = "market wire stream buffer exceeds maximum size";
        buffer_.clear();  // 无法判断帧边界时清空，避免后续合法数据被永久粘在坏帧后面。
        return false;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    return true;
}

DecodeResult StreamDecoder::next() {
    if (buffer_.empty()) {
        return {DecodeStatus::Incomplete, {}, 0, {}, {}};
    }
    DecodeResult result = decode(std::string_view(reinterpret_cast<const char*>(buffer_.data()), buffer_.size()));
    if (result.status == DecodeStatus::Ok) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + result.consumed_bytes);  // 只消费首帧，留下粘包的后续帧。
    } else if (result.status == DecodeStatus::Invalid || result.status == DecodeStatus::TooLarge) {
        buffer_.clear();  // 协议错误后不能猜测偏移量，交由上层重连恢复边界。
    }
    return result;
}

bool encode_market_event(const MarketEvent& event,
                         MarketWireMessage& output,
                         std::string& error) {
    const auto validation = validate_event(event);
    if (!validation) {
        error = validation.error;
        return false;
    }
    output = {};
    output.message_type = MessageType::Quote;
    output.source_sequence = event.source_sequence;
    output.event_time_ns = to_unix_ns(event.source_time);
    output.instrument_id = normalize_symbol(event.symbol);
    output.exchange = event.exchange;
    output.payload.assign(kBasePayloadSize, 0);
    write_double(output.payload, 0, event.last_price);
    write_u64(output.payload, 8, event.volume);
    write_double(output.payload, 16, event.turnover);
    std::size_t offset = 24;
    for (const auto& level : event.bids) {
        write_double(output.payload, offset, level.price);
        write_u64(output.payload, offset + 8, level.volume);
        offset += 16;
    }
    for (const auto& level : event.asks) {
        write_double(output.payload, offset, level.price);
        write_u64(output.payload, offset + 8, level.volume);
        offset += 16;
    }
    return true;
}

bool decode_market_event(const MarketWireMessage& message,
                         MarketEvent& output,
                         std::string& error) {
    if (!valid_event_payload(message, error)) {
        return false;
    }
    const std::string symbol = normalize_symbol(message.instrument_id);
    if (symbol.empty()) {
        error = "market wire instrument_id is not a normalized symbol";
        return false;
    }
    output = {};
    output.symbol = symbol;
    output.exchange = message.exchange;
    output.source_time = from_unix_ns(message.event_time_ns);
    output.received_at = std::chrono::system_clock::now();
    output.source_sequence = message.source_sequence;
    output.last_price = read_double(message.payload.data());
    output.volume = read_u64(message.payload.data() + 8);
    output.turnover = read_double(message.payload.data() + 16);
    std::size_t offset = 24;
    for (auto& level : output.bids) {
        level.price = read_double(message.payload.data() + offset);
        level.volume = read_u64(message.payload.data() + offset + 8);
        offset += 16;
    }
    for (auto& level : output.asks) {
        level.price = read_double(message.payload.data() + offset);
        level.volume = read_u64(message.payload.data() + offset + 8);
        offset += 16;
    }
    const auto validation = validate_event(output);
    if (!validation) {
        error = validation.error;
        return false;
    }
    return true;
}

}  // namespace oxmwh::wire
