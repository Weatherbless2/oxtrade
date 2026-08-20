#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oxmbs::ipc {

using IpcPayload = std::vector<std::uint8_t>;
using TaskSequence = std::uint64_t;
using SessionId = std::uint64_t;

enum class IpcMessageKind : std::uint16_t {
    Task = 1,
    Response = 2,
};

enum class IpcResponseStatus : std::uint32_t {
    Ok = 0,
    Rejected = 1,
    Failed = 2,
    TimedOut = 3,
};

enum class TaskProtocolStatus {
    Ok,
    InvalidArgument,
    FrameTooSmall,
    FrameTooLarge,
    InvalidMagic,
    UnsupportedVersion,
    UnexpectedMessageKind,
    InvalidHeaderSize,
    InvalidFrameSize,
    InvalidPayloadSize,
    InvalidField,
};

struct TaskProtocolLimits {
    std::size_t max_frame_bytes{4096};  // 与当前 System V 队列容量对齐，后续可由运行配置在不超过内核 MSGMAX 的范围内调整。
};

struct IpcWireLayout {
    static constexpr std::uint32_t kMagic{0x4F584D42U};  // 固定魔数 OXMB，先识别无关/损坏字节再进行字段解析。
    static constexpr std::uint16_t kVersion{1};  // 首个稳定协议版本；不兼容的帧必须被明确拒绝而不是猜测解析。
    static constexpr std::size_t kHeaderBytes{48};  // 显式逐字段编码后的固定头长度，不依赖 C++ 结构体内存对齐。

    static constexpr std::size_t kMagicOffset{0};
    static constexpr std::size_t kVersionOffset{4};
    static constexpr std::size_t kKindOffset{6};
    static constexpr std::size_t kHeaderBytesOffset{8};
    static constexpr std::size_t kFrameBytesOffset{12};
    static constexpr std::size_t kTaskSequenceOffset{16};
    static constexpr std::size_t kSessionIdOffset{24};
    static constexpr std::size_t kMessageTypeOffset{32};
    static constexpr std::size_t kFunctionIdOffset{36};
    static constexpr std::size_t kResponseStatusOffset{40};
    static constexpr std::size_t kPayloadBytesOffset{44};
};

struct IpcTaskMsg {
    TaskSequence task_seq{0};  // 主进程分配的递增任务号；0 保留为非法值，便于发现未注册任务。
    SessionId session_id{0};  // HP-Socket 等上游会话的稳定标识；Worker 只透传，不能据此访问网络对象。
    std::uint32_t msg_type{0};  // 业务消息类别，后续 WorkFunc 用它决定高低频和业务路由。
    std::uint32_t func_id{0};  // 后续 funclist/LBM 的函数标识；本工单只保证它在往返中不丢失。
    IpcPayload payload;  // 业务原始字节，可含 '\0'；协议层不假定它是 UTF-8 文本或 JSON。
};

struct IpcRespMsg {
    TaskSequence task_seq{0};  // 必须与对应请求一致，主进程用它定位待处理任务。
    SessionId session_id{0};  // 必须与对应请求一致，避免把一个客户端的结果投递给另一个会话。
    std::uint32_t msg_type{0};  // 正常情况下原样回传请求类别，帮助上层做结果类型校验。
    std::uint32_t func_id{0};  // 正常情况下原样回传请求函数标识，帮助后续 LBM 路由诊断。
    IpcResponseStatus status{IpcResponseStatus::Ok};  // Worker 执行结果，不替代主进程的本地超时状态。
    IpcPayload payload;  // 成功结果或失败详情的原始字节，长度由 wire header 中 payload_bytes 精确约束。
};

struct IpcTaskDecodeResult {
    TaskProtocolStatus status{TaskProtocolStatus::InvalidArgument};
    IpcTaskMsg message;
    std::string error;

    explicit operator bool() const noexcept { return status == TaskProtocolStatus::Ok; }
};

struct IpcRespDecodeResult {
    TaskProtocolStatus status{TaskProtocolStatus::InvalidArgument};
    IpcRespMsg message;
    std::string error;

    explicit operator bool() const noexcept { return status == TaskProtocolStatus::Ok; }
};

TaskProtocolStatus encode_task(const IpcTaskMsg& message,
                               IpcPayload& frame,
                               std::string& error,
                               TaskProtocolLimits limits = {});
IpcTaskDecodeResult decode_task(const std::uint8_t* frame,
                                std::size_t frame_size,
                                TaskProtocolLimits limits = {});

TaskProtocolStatus encode_response(const IpcRespMsg& message,
                                   IpcPayload& frame,
                                   std::string& error,
                                   TaskProtocolLimits limits = {});
IpcRespDecodeResult decode_response(const std::uint8_t* frame,
                                    std::size_t frame_size,
                                    TaskProtocolLimits limits = {});

const char* task_protocol_status_name(TaskProtocolStatus status) noexcept;

}  // namespace oxmbs::ipc
