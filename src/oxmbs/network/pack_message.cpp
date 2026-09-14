#include "oxmbs/network/pack_message.h"

#include <algorithm>
#include <array>
#include <limits>

namespace oxmbs::network {
namespace {

std::uint16_t read_u16(const std::uint8_t* data) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8U) |
                                      static_cast<std::uint16_t>(data[1]));
}

std::uint32_t read_u32(const std::uint8_t* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) |
           static_cast<std::uint32_t>(data[3]);
}

std::uint64_t read_u64(const std::uint8_t* data) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value = (value << 8U) | data[index];
    }
    return value;
}

void write_u16(std::uint8_t* data, std::uint16_t value) noexcept {
    data[0] = static_cast<std::uint8_t>(value >> 8U);
    data[1] = static_cast<std::uint8_t>(value);
}

void write_u32(std::uint8_t* data, std::uint32_t value) noexcept {
    data[0] = static_cast<std::uint8_t>(value >> 24U);
    data[1] = static_cast<std::uint8_t>(value >> 16U);
    data[2] = static_cast<std::uint8_t>(value >> 8U);
    data[3] = static_cast<std::uint8_t>(value);
}

void write_u64(std::uint8_t* data, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        data[sizeof(value) - 1U - index] = static_cast<std::uint8_t>(value >> (index * 8U));
    }
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (unsigned int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

bool valid_kind(std::uint8_t raw) noexcept {
    return raw >= static_cast<std::uint8_t>(PackMessageKind::Request) &&
           raw <= static_cast<std::uint8_t>(PackMessageKind::Heartbeat);
}

PackMessageStatus validate_limits(PackMessageLimits limits, std::string& error) {
    if (limits.max_frame_bytes < PackMessageWireLayout::kHeaderBytes ||
        limits.max_frame_bytes > PackMessageWireLayout::kDefaultMaxFrameBytes) {
        error = "max frame bytes must be between 64 and 4 MiB";
        return PackMessageStatus::InvalidArgument;
    }
    return PackMessageStatus::Ok;
}

PackMessageDecodeResult decode_header(const std::uint8_t* frame,
                                      std::size_t frame_size,
                                      PackMessageLimits limits,
                                      bool require_exact_size) {
    PackMessageDecodeResult result;
    if (frame == nullptr) {
        result.error = "frame pointer is null";
        return result;
    }
    if (const auto status = validate_limits(limits, result.error); status != PackMessageStatus::Ok) {
        result.status = status;
        return result;
    }
    if (frame_size < PackMessageWireLayout::kHeaderBytes) {
        result.status = PackMessageStatus::FrameTooSmall;
        result.error = "frame is shorter than the fixed 64-byte header";
        return result;
    }

    if (read_u32(frame + PackMessageWireLayout::kMagicOffset) != PackMessageWireLayout::kMagic) {
        result.status = PackMessageStatus::BadMagic;
        result.error = "magic is not OXPK";
        return result;
    }
    if (read_u16(frame + PackMessageWireLayout::kVersionOffset) != PackMessageWireLayout::kVersion) {
        result.status = PackMessageStatus::UnsupportedVersion;
        result.error = "unsupported PackMessage version";
        return result;
    }
    const auto header_length = read_u16(frame + PackMessageWireLayout::kHeaderLengthOffset);
    if (header_length != PackMessageWireLayout::kHeaderBytes) {
        result.status = PackMessageStatus::InvalidHeader;
        result.error = "header length must be exactly 64 bytes";
        return result;
    }
    const auto body_length = read_u32(frame + PackMessageWireLayout::kBodyLengthOffset);
    if (body_length > limits.max_frame_bytes - PackMessageWireLayout::kHeaderBytes) {
        result.status = PackMessageStatus::FrameTooLarge;
        result.error = "body length exceeds configured frame limit";
        return result;
    }
    const auto total = PackMessageWireLayout::kHeaderBytes + static_cast<std::size_t>(body_length);
    if (frame_size < total) {
        result.status = PackMessageStatus::NeedMoreData;
        result.frame_bytes = total;
        result.error = "frame payload is incomplete";
        return result;
    }
    if (require_exact_size && frame_size != total) {
        result.status = PackMessageStatus::InvalidFrameSize;
        result.error = "frame size does not match body length";
        return result;
    }
    if (frame[PackMessageWireLayout::kFlagsOffset] != 0 ||
        read_u16(frame + PackMessageWireLayout::kReservedOffset) != 0 ||
        read_u32(frame + PackMessageWireLayout::kReserved2Offset) != 0) {
        result.status = PackMessageStatus::InvalidHeader;
        result.error = "compression flags or reserved fields are non-zero";
        return result;
    }
    if (!valid_kind(frame[PackMessageWireLayout::kMessageKindOffset])) {
        result.status = PackMessageStatus::InvalidMessageKind;
        result.error = "message kind is not Request, Response, Push or Heartbeat";
        return result;
    }

    result.message.kind = static_cast<PackMessageKind>(frame[PackMessageWireLayout::kMessageKindOffset]);
    result.message.flags = frame[PackMessageWireLayout::kFlagsOffset];
    result.message.reserved = read_u16(frame + PackMessageWireLayout::kReservedOffset);
    result.message.request_id = read_u64(frame + PackMessageWireLayout::kRequestIdOffset);
    result.message.session_id = read_u64(frame + PackMessageWireLayout::kSessionIdOffset);
    result.message.message_type = read_u16(frame + PackMessageWireLayout::kMessageTypeOffset);
    result.message.function_id = read_u16(frame + PackMessageWireLayout::kFunctionIdOffset);
    result.message.sequence = read_u64(frame + PackMessageWireLayout::kSequenceOffset);
    result.message.timestamp_ms = read_u64(frame + PackMessageWireLayout::kTimestampMsOffset);
    result.message.error_code = read_u32(frame + PackMessageWireLayout::kErrorCodeOffset);
    result.message.reserved2 = read_u32(frame + PackMessageWireLayout::kReserved2Offset);
    result.frame_bytes = total;
    result.status = PackMessageStatus::Ok;
    return result;
}

PackMessageStatus validate_message_fields(const PackMessage& message, std::string& error) {
    if (!valid_kind(static_cast<std::uint8_t>(message.kind))) {
        error = "message kind is not Request, Response, Push or Heartbeat";
        return PackMessageStatus::InvalidMessageKind;
    }
    if (message.flags != 0 || message.reserved != 0 || message.reserved2 != 0) {
        error = "compression flags or reserved fields are non-zero";
        return PackMessageStatus::InvalidHeader;
    }
    if (message.kind == PackMessageKind::Request && message.error_code != 0) {
        error = "Request error_code must be zero";
        return PackMessageStatus::InvalidHeader;
    }
    if (message.kind == PackMessageKind::Heartbeat && message.function_id != 0) {
        error = "Heartbeat function_id must be zero";
        return PackMessageStatus::InvalidHeader;
    }
    if (message.payload.empty() && message.error_code == 0) {
        return PackMessageStatus::Ok;
    }
    return PackMessageStatus::Ok;
}

}  // namespace

PackMessageStatus encode_pack_message(const PackMessage& message,
                                      PackPayload& frame,
                                      std::string& error,
                                      PackMessageLimits limits) {
    frame.clear();
    if (const auto status = validate_limits(limits, error); status != PackMessageStatus::Ok) {
        return status;
    }
    if (const auto status = validate_message_fields(message, error); status != PackMessageStatus::Ok) {
        return status;
    }
    if (message.payload.size() > limits.max_frame_bytes - PackMessageWireLayout::kHeaderBytes ||
        message.payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = "payload exceeds configured frame limit";
        return PackMessageStatus::FrameTooLarge;
    }

    const auto body_length = static_cast<std::uint32_t>(message.payload.size());
    const auto frame_size = PackMessageWireLayout::kHeaderBytes + message.payload.size();
    frame.assign(frame_size, 0);  // 先分配完整帧，随后按字段写入网络字节序，绝不依赖结构体对齐。
    write_u32(frame.data() + PackMessageWireLayout::kMagicOffset, PackMessageWireLayout::kMagic);
    write_u16(frame.data() + PackMessageWireLayout::kVersionOffset, PackMessageWireLayout::kVersion);
    write_u16(frame.data() + PackMessageWireLayout::kHeaderLengthOffset,
              static_cast<std::uint16_t>(PackMessageWireLayout::kHeaderBytes));
    write_u32(frame.data() + PackMessageWireLayout::kBodyLengthOffset, body_length);
    frame[PackMessageWireLayout::kMessageKindOffset] = static_cast<std::uint8_t>(message.kind);
    frame[PackMessageWireLayout::kFlagsOffset] = message.flags;
    write_u16(frame.data() + PackMessageWireLayout::kReservedOffset, message.reserved);
    write_u64(frame.data() + PackMessageWireLayout::kRequestIdOffset, message.request_id);
    write_u64(frame.data() + PackMessageWireLayout::kSessionIdOffset, message.session_id);
    write_u16(frame.data() + PackMessageWireLayout::kMessageTypeOffset, message.message_type);
    write_u16(frame.data() + PackMessageWireLayout::kFunctionIdOffset, message.function_id);
    write_u64(frame.data() + PackMessageWireLayout::kSequenceOffset, message.sequence);
    write_u64(frame.data() + PackMessageWireLayout::kTimestampMsOffset, message.timestamp_ms);
    write_u32(frame.data() + PackMessageWireLayout::kErrorCodeOffset, message.error_code);
    const auto checksum = message.payload.empty() ? 0U : crc32(message.payload.data(), message.payload.size());
    write_u32(frame.data() + PackMessageWireLayout::kPayloadCrc32Offset, checksum);
    write_u32(frame.data() + PackMessageWireLayout::kReserved2Offset, message.reserved2);
    std::copy(message.payload.begin(), message.payload.end(),
              frame.begin() + static_cast<std::ptrdiff_t>(PackMessageWireLayout::kHeaderBytes));
    return PackMessageStatus::Ok;
}

PackMessageDecodeResult decode_pack_message(const std::uint8_t* frame,
                                            std::size_t frame_size,
                                            PackMessageLimits limits) {
    auto result = decode_header(frame, frame_size, limits, true);
    if (!result) {
        return result;
    }

    const auto payload_size = result.frame_bytes - PackMessageWireLayout::kHeaderBytes;
    const auto* payload = frame + PackMessageWireLayout::kHeaderBytes;
    const auto expected_crc = read_u32(frame + PackMessageWireLayout::kPayloadCrc32Offset);
    if ((payload_size == 0 && expected_crc != 0) ||
        (payload_size != 0 && crc32(payload, payload_size) != expected_crc)) {
        result.status = PackMessageStatus::InvalidPayload;
        result.error = "payload CRC32 does not match";
        return result;
    }
    if (result.message.kind == PackMessageKind::Request && result.message.error_code != 0) {
        result.status = PackMessageStatus::InvalidHeader;
        result.error = "Request error_code must be zero";
        return result;
    }
    if (result.message.kind == PackMessageKind::Heartbeat && result.message.function_id != 0) {
        result.status = PackMessageStatus::InvalidHeader;
        result.error = "Heartbeat function_id must be zero";
        return result;
    }
    result.message.payload.assign(payload, payload + payload_size);  // 解码完成后才复制 payload，坏帧不会泄露半成品。
    result.status = PackMessageStatus::Ok;
    return result;
}

const char* pack_message_status_name(PackMessageStatus status) noexcept {
    switch (status) {
    case PackMessageStatus::Ok: return "Ok";
    case PackMessageStatus::InvalidArgument: return "InvalidArgument";
    case PackMessageStatus::NeedMoreData: return "NeedMoreData";
    case PackMessageStatus::FrameTooSmall: return "FrameTooSmall";
    case PackMessageStatus::FrameTooLarge: return "FrameTooLarge";
    case PackMessageStatus::BadMagic: return "BadMagic";
    case PackMessageStatus::UnsupportedVersion: return "UnsupportedVersion";
    case PackMessageStatus::InvalidHeader: return "InvalidHeader";
    case PackMessageStatus::InvalidFrameSize: return "InvalidFrameSize";
    case PackMessageStatus::InvalidPayload: return "InvalidPayload";
    case PackMessageStatus::InvalidMessageKind: return "InvalidMessageKind";
    }
    return "Unknown";
}

PackMessageFrameDecoder::PackMessageFrameDecoder(PackMessageLimits limits)
    : limits_(limits) {
}

PackMessageStatus PackMessageFrameDecoder::append(const std::uint8_t* data,
                                                  std::size_t size,
                                                  std::vector<PackMessage>& messages,
                                                  std::string& error) {
    messages.clear();
    if (data == nullptr && size != 0) {
        error = "receive data pointer is null";
        return PackMessageStatus::InvalidArgument;
    }
    if (size > limits_.max_frame_bytes || buffer_.size() > limits_.max_frame_bytes - size) {
        error = "receive buffer exceeds configured frame limit";
        return PackMessageStatus::FrameTooLarge;
    }
    if (size != 0) {
        buffer_.insert(buffer_.end(), data, data + size);
    }

    while (!buffer_.empty()) {
        if (buffer_.size() < PackMessageWireLayout::kHeaderBytes) {
            return PackMessageStatus::NeedMoreData;
        }
        const auto header = decode_header(buffer_.data(), buffer_.size(), limits_, false);
        if (header.status == PackMessageStatus::NeedMoreData) {
            return PackMessageStatus::NeedMoreData;
        }
        if (!header) {
            error = header.error;
            buffer_.clear();  // 当前连接会被关闭；清空避免坏帧与下一次连接数据混合。
            return header.status;
        }
        const auto decoded = decode_pack_message(buffer_.data(), header.frame_bytes, limits_);
        if (!decoded) {
            error = decoded.error;
            buffer_.clear();
            return decoded.status;
        }
        messages.push_back(decoded.message);
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(header.frame_bytes));
    }
    return messages.empty() ? PackMessageStatus::NeedMoreData : PackMessageStatus::Ok;
}

void PackMessageFrameDecoder::clear() noexcept {
    buffer_.clear();
}

std::size_t PackMessageFrameDecoder::buffered_bytes() const noexcept {
    return buffer_.size();
}

}  // namespace oxmbs::network
