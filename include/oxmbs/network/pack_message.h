#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oxmbs::network {

using PackPayload = std::vector<std::uint8_t>;

enum class PackMessageKind : std::uint8_t {
    Request = 1,
    Response = 2,
    Push = 3,
    Heartbeat = 4,
};

enum class PackMessageErrorCode : std::uint32_t {
    Ok = 0,
    BadMagic = 1001,
    FrameTooLarge = 1002,
    UnsupportedVersion = 1003,
    InvalidHeader = 1004,
    InvalidPayload = 1005,
    RateLimited = 1006,
    NotAuthenticated = 1101,
    AuthFailed = 1102,
    SessionExpired = 1103,
    SessionMismatch = 1104,
    PermissionDenied = 1105,
    DuplicateRequest = 1201,
    RequestTimeout = 1202,
    StaleResponse = 1203,
    InvalidArgument = 2001,
    EmptyResult = 2002,
    FunctionNotAvailable = 2003,
    InternalError = 9001,
};

struct PackMessageWireLayout {
    static constexpr std::uint32_t kMagic = 0x4F58504BU;  // ASCII OXPK；先识别协议族再解析后续字段。
    static constexpr std::uint16_t kVersion = 1;
    static constexpr std::size_t kHeaderBytes = 64;
    static constexpr std::size_t kDefaultMaxFrameBytes = 4U * 1024U * 1024U;

    static constexpr std::size_t kMagicOffset = 0;
    static constexpr std::size_t kVersionOffset = 4;
    static constexpr std::size_t kHeaderLengthOffset = 6;
    static constexpr std::size_t kBodyLengthOffset = 8;
    static constexpr std::size_t kMessageKindOffset = 12;
    static constexpr std::size_t kFlagsOffset = 13;
    static constexpr std::size_t kReservedOffset = 14;
    static constexpr std::size_t kRequestIdOffset = 16;
    static constexpr std::size_t kSessionIdOffset = 24;
    static constexpr std::size_t kMessageTypeOffset = 32;
    static constexpr std::size_t kFunctionIdOffset = 34;
    static constexpr std::size_t kSequenceOffset = 36;
    static constexpr std::size_t kTimestampMsOffset = 44;
    static constexpr std::size_t kErrorCodeOffset = 52;
    static constexpr std::size_t kPayloadCrc32Offset = 56;
    static constexpr std::size_t kReserved2Offset = 60;
};

struct PackMessageLimits {
    std::size_t max_frame_bytes{PackMessageWireLayout::kDefaultMaxFrameBytes};
};

struct PackMessage {
    PackMessageKind kind{PackMessageKind::Request};
    std::uint8_t flags{0};
    std::uint16_t reserved{0};
    std::uint64_t request_id{0};
    std::uint64_t session_id{0};
    std::uint16_t message_type{0};
    std::uint16_t function_id{0};
    std::uint64_t sequence{0};
    std::uint64_t timestamp_ms{0};
    std::uint32_t error_code{0};
    std::uint32_t reserved2{0};
    PackPayload payload;
};

enum class PackMessageStatus {
    Ok,
    InvalidArgument,
    NeedMoreData,
    FrameTooSmall,
    FrameTooLarge,
    BadMagic,
    UnsupportedVersion,
    InvalidHeader,
    InvalidFrameSize,
    InvalidPayload,
    InvalidMessageKind,
};

struct PackMessageDecodeResult {
    PackMessageStatus status{PackMessageStatus::InvalidArgument};
    PackMessage message;
    std::size_t frame_bytes{0};
    std::string error;

    explicit operator bool() const noexcept { return status == PackMessageStatus::Ok; }
};

PackMessageStatus encode_pack_message(const PackMessage& message,
                                      PackPayload& frame,
                                      std::string& error,
                                      PackMessageLimits limits = {});

PackMessageDecodeResult decode_pack_message(const std::uint8_t* frame,
                                            std::size_t frame_size,
                                            PackMessageLimits limits = {});

const char* pack_message_status_name(PackMessageStatus status) noexcept;

class PackMessageFrameDecoder {
public:
    explicit PackMessageFrameDecoder(PackMessageLimits limits = {});

    PackMessageStatus append(const std::uint8_t* data,
                             std::size_t size,
                             std::vector<PackMessage>& messages,
                             std::string& error);
    void clear() noexcept;
    std::size_t buffered_bytes() const noexcept;

private:
    PackMessageLimits limits_;
    PackPayload buffer_;
};

}  // namespace oxmbs::network
