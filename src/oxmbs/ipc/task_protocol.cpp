#include "oxmbs/ipc/task_protocol.h"

#include <limits>
#include <utility>

namespace oxmbs::ipc {
namespace {

struct DecodedHeader {
    // 解码阶段先只保存已验证的固定头字段；payload 要等长度、版本、类型和字段全通过后才复制到业务对象。
    IpcMessageKind kind{IpcMessageKind::Task};
    TaskSequence task_seq{0};
    SessionId session_id{0};
    std::uint64_t deadline_unix_ms{0};
    std::uint32_t msg_type{0};
    std::uint32_t func_id{0};
    std::uint32_t response_status{0};
    std::size_t payload_bytes{0};
};

bool limits_are_valid(const TaskProtocolLimits& limits) noexcept {
    // 小于固定头会让“合法空 payload 帧”也无法容纳；大于 uint32_t 则无法写入本协议的长度字段。
    return limits.max_frame_bytes >= IpcWireLayout::kHeaderBytes &&
           limits.max_frame_bytes <= std::numeric_limits<std::uint32_t>::max();
}

bool is_valid_response_status(std::uint32_t value) noexcept {
    // 不接受未知枚举值，避免不同版本把同一个整数解释成不同业务结果。
    switch (static_cast<IpcResponseStatus>(value)) {
    case IpcResponseStatus::Ok:
    case IpcResponseStatus::Rejected:
    case IpcResponseStatus::Failed:
    case IpcResponseStatus::TimedOut:
        return true;
    }
    return false;
}

void append_u16(IpcPayload& target, std::uint16_t value) {
    target.push_back(static_cast<std::uint8_t>(value & 0xFFU));  // wire 固定小端序，Windows 适配时也不依赖当前 CPU 的结构体布局。
    target.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void append_u32(IpcPayload& target, std::uint32_t value) {
    // 每次只写一个低位字节，明确固定小端 wire 格式，而不是把宿主整数的内存直接 memcpy 到跨进程帧中。
    for (std::uint32_t byte_index = 0; byte_index < 4; ++byte_index) {
        target.push_back(static_cast<std::uint8_t>((value >> (byte_index * 8U)) & 0xFFU));
    }
}

void append_u64(IpcPayload& target, std::uint64_t value) {
    // task_seq/session_id 固定占 8 字节；循环按低位到高位追加，32/64 位进程得到相同字节布局。
    for (std::uint32_t byte_index = 0; byte_index < 8; ++byte_index) {
        target.push_back(static_cast<std::uint8_t>((value >> (byte_index * 8U)) & 0xFFU));
    }
}

std::uint16_t read_u16(const std::uint8_t* source, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(source[offset]) |
                                      (static_cast<std::uint16_t>(source[offset + 1]) << 8U));
}

std::uint32_t read_u32(const std::uint8_t* source, std::size_t offset) noexcept {
    // 调用处已保证固定头完整，按与 append_u32 相反的顺序组装，不依赖 CPU 当前大小端。
    std::uint32_t value{0};
    for (std::uint32_t byte_index = 0; byte_index < 4; ++byte_index) {
        value |= static_cast<std::uint32_t>(source[offset + byte_index]) << (byte_index * 8U);
    }
    return value;
}

std::uint64_t read_u64(const std::uint8_t* source, std::size_t offset) noexcept {
    // 关联字段需要完整恢复 64 位值，不能用指针转换读取，否则可能遇到对齐和端序问题。
    std::uint64_t value{0};
    for (std::uint32_t byte_index = 0; byte_index < 8; ++byte_index) {
        value |= static_cast<std::uint64_t>(source[offset + byte_index]) << (byte_index * 8U);
    }
    return value;
}

TaskProtocolStatus encode_message(IpcMessageKind kind,
                                  TaskSequence task_seq,
                                  SessionId session_id,
                                  std::uint64_t deadline_unix_ms,
                                  std::uint32_t msg_type,
                                  std::uint32_t func_id,
                                  std::uint32_t response_status,
                                  const IpcPayload& payload,
                                  IpcPayload& frame,
                                  std::string& error,
                                  TaskProtocolLimits limits) {
    // Task 和 Response 共用同一组帧规则；两个公开入口只负责固定 kind 与 response_status 的差异。
    frame.clear();  // 编码失败不能留下可被误发送的半帧，调用方只能在 Ok 后把 frame 交给队列。
    error.clear();
    if (!limits_are_valid(limits)) {  // 防止长度限制本身无法表示协议固定头，失败时没有任何资源或半帧需要回滚。
        error = "max_frame_bytes must be between the header size and uint32_t maximum";
        return TaskProtocolStatus::InvalidArgument;
    }
    if (task_seq == 0 || session_id == 0 || deadline_unix_ms == 0 || msg_type == 0) {  // 零 deadline 无法让 Worker 判断是否过期，必须在进队前拦截。
        error = "task_seq, session_id, deadline_unix_ms and msg_type must be non-zero";
        return TaskProtocolStatus::InvalidField;
    }
    if (kind == IpcMessageKind::Task && response_status != 0) {
        error = "task frames must reserve the response status field as zero";
        return TaskProtocolStatus::InvalidField;
    }
    if (kind == IpcMessageKind::Response && !is_valid_response_status(response_status)) {
        error = "response status is not supported by this protocol version";
        return TaskProtocolStatus::InvalidField;
    }
    // 先做减法形式的溢出检查，再相加得到 frame_bytes，避免超大 payload 回绕成看似很小的帧。
    if (payload.size() > std::numeric_limits<std::uint32_t>::max() - IpcWireLayout::kHeaderBytes) {
        error = "payload is too large for the uint32_t wire length fields";
        return TaskProtocolStatus::FrameTooLarge;
    }

    const std::size_t frame_bytes = IpcWireLayout::kHeaderBytes + payload.size();  // 业务帧长度，不含工单 #4 System V 的 long mtype 头。
    if (frame_bytes > limits.max_frame_bytes) {
        error = "encoded frame exceeds the configured maximum";
        return TaskProtocolStatus::FrameTooLarge;
    }

    IpcPayload encoded;
    encoded.reserve(frame_bytes);  // 一次性分配固定头与正文，避免逐字段追加时反复扩容并改变性能特征。
    // 以下字段顺序必须与 IpcWireLayout 偏移表完全一致：固定头之后才附加原始 payload。
    append_u32(encoded, IpcWireLayout::kMagic);
    append_u16(encoded, IpcWireLayout::kVersion);
    append_u16(encoded, static_cast<std::uint16_t>(kind));
    append_u16(encoded, static_cast<std::uint16_t>(IpcWireLayout::kHeaderBytes));
    append_u16(encoded, 0);  // 保留字段必须为 0；未来扩展需要升级版本而不是复用旧语义。
    append_u32(encoded, static_cast<std::uint32_t>(frame_bytes));
    append_u64(encoded, task_seq);
    append_u64(encoded, session_id);
    append_u64(encoded, deadline_unix_ms);  // 绝对 Unix 毫秒值独立写入 wire 帧，fork 后的 Worker 不需要访问主进程 registry。
    append_u32(encoded, msg_type);
    append_u32(encoded, func_id);
    append_u32(encoded, response_status);
    append_u32(encoded, static_cast<std::uint32_t>(payload.size()));
    encoded.insert(encoded.end(), payload.begin(), payload.end());  // payload 原样复制，可安全包含 0 字节和非文本数据。

    frame = std::move(encoded);  // 所有校验和组包完成后才发布完整帧给调用方。
    return TaskProtocolStatus::Ok;
}

TaskProtocolStatus decode_header(const std::uint8_t* frame,
                                 std::size_t frame_size,
                                 IpcMessageKind expected_kind,
                                 TaskProtocolLimits limits,
                                 DecodedHeader& header,
                                 std::string& error) {
    // 解码顺序由“最便宜且最能阻止越界读取”的检查开始；失败立即返回，绝不尝试恢复或猜测损坏帧。
    if (frame == nullptr || frame_size == 0 || !limits_are_valid(limits)) {
        error = "frame pointer, frame size or protocol limits are invalid";
        return TaskProtocolStatus::InvalidArgument;
    }
    if (frame_size < IpcWireLayout::kHeaderBytes) {  // 在读 magic/version 前先保证每个固定偏移都在收到的正文范围内。
        error = "frame is shorter than the fixed protocol header";
        return TaskProtocolStatus::FrameTooSmall;
    }
    if (frame_size > limits.max_frame_bytes) {
        error = "received frame exceeds the configured maximum";
        return TaskProtocolStatus::FrameTooLarge;
    }
    if (read_u32(frame, IpcWireLayout::kMagicOffset) != IpcWireLayout::kMagic) {
        error = "protocol magic does not match oxMBS IPC";
        return TaskProtocolStatus::InvalidMagic;
    }
    if (read_u16(frame, IpcWireLayout::kVersionOffset) != IpcWireLayout::kVersion) {
        error = "protocol version is not supported";
        return TaskProtocolStatus::UnsupportedVersion;
    }

    const auto raw_kind = read_u16(frame, IpcWireLayout::kKindOffset);  // Request/Reply 使用独立解码入口，方向错位不能混入后续处理。
    if (raw_kind != static_cast<std::uint16_t>(expected_kind)) {
        error = "frame kind is not valid for this decoder";
        return TaskProtocolStatus::UnexpectedMessageKind;
    }
    if (read_u16(frame, IpcWireLayout::kHeaderBytesOffset) != IpcWireLayout::kHeaderBytes ||
        read_u16(frame, 10) != 0) {  // 字节 10-11 为保留字段，非 0 说明发送端没有遵守当前版本的固定头约定。
        error = "header size or reserved header field is invalid";
        return TaskProtocolStatus::InvalidHeaderSize;
    }

    // System V receive 返回的 frame_size 是可信边界；头内两个长度必须同时与它吻合，防止截断、拼接或伪造 payload。
    const auto declared_frame_bytes = static_cast<std::size_t>(read_u32(frame, IpcWireLayout::kFrameBytesOffset));
    const auto declared_payload_bytes = static_cast<std::size_t>(read_u32(frame, IpcWireLayout::kPayloadBytesOffset));
    if (declared_frame_bytes != frame_size || declared_frame_bytes < IpcWireLayout::kHeaderBytes) {
        error = "declared frame length does not equal received byte count";
        return TaskProtocolStatus::InvalidFrameSize;
    }
    if (declared_payload_bytes != frame_size - IpcWireLayout::kHeaderBytes) {
        error = "declared payload length does not equal frame remainder";
        return TaskProtocolStatus::InvalidPayloadSize;
    }

    header.kind = expected_kind;
    header.task_seq = read_u64(frame, IpcWireLayout::kTaskSequenceOffset);
    header.session_id = read_u64(frame, IpcWireLayout::kSessionIdOffset);
    header.deadline_unix_ms = read_u64(frame, IpcWireLayout::kDeadlineUnixMsOffset);
    header.msg_type = read_u32(frame, IpcWireLayout::kMessageTypeOffset);
    header.func_id = read_u32(frame, IpcWireLayout::kFunctionIdOffset);
    header.response_status = read_u32(frame, IpcWireLayout::kResponseStatusOffset);
    header.payload_bytes = declared_payload_bytes;

    if (header.task_seq == 0 || header.session_id == 0 || header.deadline_unix_ms == 0 || header.msg_type == 0) {
        error = "task_seq, session_id, deadline_unix_ms and msg_type must be non-zero";
        return TaskProtocolStatus::InvalidField;
    }
    if (expected_kind == IpcMessageKind::Task && header.response_status != 0) {
        error = "task frame contains a non-zero response status";
        return TaskProtocolStatus::InvalidField;
    }
    if (expected_kind == IpcMessageKind::Response && !is_valid_response_status(header.response_status)) {
        error = "response frame contains an unsupported response status";
        return TaskProtocolStatus::InvalidField;
    }
    return TaskProtocolStatus::Ok;
}

}  // namespace

TaskProtocolStatus encode_task(const IpcTaskMsg& message,
                               IpcPayload& frame,
                               std::string& error,
                               TaskProtocolLimits limits) {
    // Task 固定写 kind=Task 且 response_status=0，业务调用者不能误把 Response 状态塞进请求帧。
    return encode_message(IpcMessageKind::Task,
                          message.task_seq,
                          message.session_id,
                          message.deadline_unix_ms,
                          message.msg_type,
                          message.func_id,
                          0,
                          message.payload,
                          frame,
                          error,
                          limits);
}

IpcTaskDecodeResult decode_task(const std::uint8_t* frame, std::size_t frame_size, TaskProtocolLimits limits) {
    IpcTaskDecodeResult result;
    DecodedHeader header;
    result.status = decode_header(frame, frame_size, IpcMessageKind::Task, limits, header, result.error);  // 完整验证通过前 result.message 保持默认值。
    if (result.status != TaskProtocolStatus::Ok) {
        return result;
    }

    result.message.task_seq = header.task_seq;
    result.message.session_id = header.session_id;
    result.message.deadline_unix_ms = header.deadline_unix_ms;
    result.message.msg_type = header.msg_type;
    result.message.func_id = header.func_id;
    result.message.payload.assign(frame + IpcWireLayout::kHeaderBytes, frame + frame_size);  // 仅在全部头校验通过后复制正文，拒绝帧不污染业务对象。
    result.status = TaskProtocolStatus::Ok;
    return result;
}

TaskProtocolStatus encode_response(const IpcRespMsg& message,
                                   IpcPayload& frame,
                                   std::string& error,
                                   TaskProtocolLimits limits) {
    // Response 复用请求的关联字段，只有 status 和 payload 表示 Worker 的处理结果。
    return encode_message(IpcMessageKind::Response,
                          message.task_seq,
                          message.session_id,
                          message.deadline_unix_ms,
                          message.msg_type,
                          message.func_id,
                          static_cast<std::uint32_t>(message.status),
                          message.payload,
                          frame,
                          error,
                          limits);
}

IpcRespDecodeResult decode_response(const std::uint8_t* frame, std::size_t frame_size, TaskProtocolLimits limits) {
    IpcRespDecodeResult result;
    DecodedHeader header;
    result.status = decode_header(frame, frame_size, IpcMessageKind::Response, limits, header, result.error);  // 先确认 Reply 真的是本版本 Response，再允许关联表消费它。
    if (result.status != TaskProtocolStatus::Ok) {
        return result;
    }

    result.message.task_seq = header.task_seq;
    result.message.session_id = header.session_id;
    result.message.deadline_unix_ms = header.deadline_unix_ms;
    result.message.msg_type = header.msg_type;
    result.message.func_id = header.func_id;
    result.message.status = static_cast<IpcResponseStatus>(header.response_status);
    result.message.payload.assign(frame + IpcWireLayout::kHeaderBytes, frame + frame_size);  // Response 与 Task 使用同一固定头，区别只在 kind 和 status 的严格校验。
    result.status = TaskProtocolStatus::Ok;
    return result;
}

const char* task_protocol_status_name(TaskProtocolStatus status) noexcept {
    switch (status) {
    case TaskProtocolStatus::Ok:
        return "Ok";
    case TaskProtocolStatus::InvalidArgument:
        return "InvalidArgument";
    case TaskProtocolStatus::FrameTooSmall:
        return "FrameTooSmall";
    case TaskProtocolStatus::FrameTooLarge:
        return "FrameTooLarge";
    case TaskProtocolStatus::InvalidMagic:
        return "InvalidMagic";
    case TaskProtocolStatus::UnsupportedVersion:
        return "UnsupportedVersion";
    case TaskProtocolStatus::UnexpectedMessageKind:
        return "UnexpectedMessageKind";
    case TaskProtocolStatus::InvalidHeaderSize:
        return "InvalidHeaderSize";
    case TaskProtocolStatus::InvalidFrameSize:
        return "InvalidFrameSize";
    case TaskProtocolStatus::InvalidPayloadSize:
        return "InvalidPayloadSize";
    case TaskProtocolStatus::InvalidField:
        return "InvalidField";
    }
    return "Unknown";
}

}  // namespace oxmbs::ipc
