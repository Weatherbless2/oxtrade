#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oxmbs::ipc {

using IpcPayload = std::vector<std::uint8_t>;  // 协议正文按字节保存，可包含 0，不能用 C 字符串长度解释。
using TaskSequence = std::uint64_t;  // 主进程全局递增的请求编号，用于把异步应答找回原请求。
using SessionId = std::uint64_t;  // 上游连接/会话标识，只在主进程用于定向回包，Worker 不拥有其生命周期。

enum class IpcMessageKind : std::uint16_t {
    Task = 1,      // Request Queue 中的请求帧；response_status 字段必须为 0。
    Response = 2,  // Reply Queue 中的应答帧；携带 Worker 的执行状态。
};

enum class IpcResponseStatus : std::uint32_t {
    Ok = 0,        // Worker 已处理成功，payload 是正常结果字节。
    Rejected = 1,  // Worker 或后续路由因业务前置条件拒绝请求。
    Failed = 2,    // Worker 已开始处理但执行失败，payload 可携带失败详情。
    TimedOut = 3,  // Worker 侧处理超时；与主进程关联表中的本地超时是两个独立来源。
};

enum class TaskProtocolStatus {
    Ok,                    // 编码得到完整帧，或解码后业务对象可被上层使用。
    InvalidArgument,       // 空指针、零长度或不合法协议限制等调用前置条件失败。
    FrameTooSmall,         // 实收字节不足 v2 固定 56 字节头，任何字段都不能安全读取。
    FrameTooLarge,         // 编码或实收帧超过当前队列/协议允许的最大长度。
    InvalidMagic,          // 不是 oxMBS 协议帧或首部已损坏。
    UnsupportedVersion,    // 帧使用当前实现不认识的版本，不能猜测字段语义。
    UnexpectedMessageKind, // Task 解码入口收到 Response，或 Response 解码入口收到 Task。
    InvalidHeaderSize,     // 固定头长度或保留字段不符合本协议版本。
    InvalidFrameSize,      // 头内 frame_bytes 与 System V 实际接收字节数不一致。
    InvalidPayloadSize,    // 头内 payload_bytes 与 frame 剩余字节数不一致。
    InvalidField,          // task_seq、session_id、msg_type 或 status 等业务字段非法。
};

struct IpcWireLayout {
    static constexpr std::uint32_t kMagic{0x4F584D42U};  // 固定魔数 OXMB，先识别无关/损坏字节再进行字段解析。
    static constexpr std::uint16_t kVersion{2};  // v2 增加跨进程 deadline；v1 没有该字段，必须拒绝而不是猜测补默认值。
    static constexpr std::size_t kHeaderBytes{56};  // 显式逐字段编码后的固定头长度，不依赖 C++ 结构体内存对齐；payload 从偏移 56 开始。

    static constexpr std::size_t kMagicOffset{0};
    static constexpr std::size_t kVersionOffset{4};
    static constexpr std::size_t kKindOffset{6};
    static constexpr std::size_t kHeaderBytesOffset{8};
    static constexpr std::size_t kFrameBytesOffset{12};
    static constexpr std::size_t kTaskSequenceOffset{16};
    static constexpr std::size_t kSessionIdOffset{24};
    static constexpr std::size_t kDeadlineUnixMsOffset{32};
    static constexpr std::size_t kMessageTypeOffset{40};
    static constexpr std::size_t kFunctionIdOffset{44};
    static constexpr std::size_t kResponseStatusOffset{48};
    static constexpr std::size_t kPayloadBytesOffset{52};
};

std::size_t systemv_msgmax_bytes() noexcept;
std::size_t default_task_frame_bytes() noexcept;

struct TaskProtocolLimits {
    std::size_t max_frame_bytes{default_task_frame_bytes()};  // Runtime cap is bounded by Linux System V MSGMAX.
};

struct IpcTaskMsg {
    TaskSequence task_seq{0};  // 主进程分配的递增任务号；0 保留为非法值，便于发现未注册任务。
    SessionId session_id{0};  // HP-Socket 等上游会话的稳定标识；Worker 只透传，不能据此访问网络对象。
    std::uint64_t deadline_unix_ms{0};  // 主进程写入的绝对 Unix 毫秒截止时间；Worker 用它在没有主进程内存的情况下丢弃过期任务。
    std::uint32_t msg_type{0};  // 业务消息类别，后续 WorkFunc 用它决定高低频和业务路由。
    std::uint32_t func_id{0};  // 后续 funclist/LBM 的函数标识；本工单只保证它在往返中不丢失。
    IpcPayload payload;  // 业务原始字节，可含 '\0'；协议层不假定它是 UTF-8 文本或 JSON。
};

struct IpcRespMsg {
    TaskSequence task_seq{0};  // 必须与对应请求一致，主进程用它定位待处理任务。
    SessionId session_id{0};  // 必须与对应请求一致，避免把一个客户端的结果投递给另一个会话。
    std::uint64_t deadline_unix_ms{0};  // 从请求原样透传，便于主进程诊断迟到 Reply，不能由 Worker 延长。
    std::uint32_t msg_type{0};  // 正常情况下原样回传请求类别，帮助上层做结果类型校验。
    std::uint32_t func_id{0};  // 正常情况下原样回传请求函数标识，帮助后续 LBM 路由诊断。
    IpcResponseStatus status{IpcResponseStatus::Ok};  // Worker 执行结果，不替代主进程的本地超时状态。
    IpcPayload payload;  // 成功结果或失败详情的原始字节，长度由 wire header 中 payload_bytes 精确约束。
    std::uint64_t request_id{0};  // 只在主进程关联表中回填，不进入 IPC wire；用于把响应还原到原始 PackMessage 请求。
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
                               TaskProtocolLimits limits = {});  // 失败时清空 frame，调用方只能在 Ok 后把它交给 Request Queue。
IpcTaskDecodeResult decode_task(const std::uint8_t* frame,
                                std::size_t frame_size,
                                TaskProtocolLimits limits = {});  // 先验证全部头与长度，再复制 payload，拒绝帧不产生部分业务对象。

TaskProtocolStatus encode_response(const IpcRespMsg& message,
                                   IpcPayload& frame,
                                   std::string& error,
                                   TaskProtocolLimits limits = {});  // 保留请求的 task_seq/session_id/deadline，让主进程按原会话和原截止时间匹配 Reply。
IpcRespDecodeResult decode_response(const std::uint8_t* frame,
                                    std::size_t frame_size,
                                    TaskProtocolLimits limits = {});  // Response 除固定头校验外，还验证 status 属于当前版本支持的枚举。

const char* task_protocol_status_name(TaskProtocolStatus status) noexcept;

}  // namespace oxmbs::ipc
