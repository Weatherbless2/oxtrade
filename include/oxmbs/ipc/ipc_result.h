#pragma once

#include <cstddef>
#include <string>

namespace oxmbs::ipc {

enum class IpcResult {
    Ok,                  // 系统调用完成；send 表示正文已交给内核，receive 表示正文已复制到调用方缓冲区。
    InvalidArgument,     // queue id、mtype、正文指针/长度、容量或创建参数不符合本封装的前置条件。
    NotOpen,             // C++ 对象没有有效 queue id；调用方应先由 manager open()，不能直接重试收发。
    Interrupted,         // 阻塞 msgsnd/msgrcv 被信号打断；消息是否已完成未知，由上层决定重试策略。
    WouldBlock,          // 使用 IPC_NOWAIT 时当前不能立即完成；消息未取出/未发送，调用方可稍后轮询。
    MessageTooLarge,     // 接收 buffer 小于队列中正文；本次消息未被取走，需用更大 buffer 重新接收。
    QueueRemoved,        // 队列被 IPC_RMID 删除；阻塞等待被唤醒，旧 queue id 不可继续使用。
    PermissionDenied,    // 当前进程权限不满足队列读写要求，继续重试通常不会改变结果。
    ResourceUnavailable, // 内核 IPC 资源不足；属于主机容量问题，应告警或降级，而不是无限重试。
    SystemError,         // 暂未细分的系统错误；同时查看 ReceiveResult::error 或 error 参数中的 strerror 文本。
};

const char* ipc_result_name(IpcResult result) noexcept;

struct ReceiveResult {
    IpcResult result{IpcResult::SystemError};
    std::size_t size{0};
    long message_type{0};
    std::string error;

    explicit operator bool() const noexcept {  // 只有正文已成功取出并复制到 buffer 时，调用方才可读取 size/type。
        return result == IpcResult::Ok;
    }
};

}  // namespace oxmbs::ipc
