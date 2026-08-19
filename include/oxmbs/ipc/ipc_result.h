#pragma once

#include <cstddef>
#include <string>

namespace oxmbs::ipc {

enum class IpcResult {
    Ok,                  // 系统调用成功，消息已发送或已接收。
    InvalidArgument,     // queue id、消息类型、长度或配置参数不合法。
    NotOpen,             // 当前 C++ 包装对象没有持有有效的 System V queue id。
    Interrupted,         // 阻塞系统调用被信号打断，调用方决定是否重试。
    WouldBlock,          // 非阻塞模式下暂时没有消息或队列空间不足。
    MessageTooLarge,     // 接收缓冲区不足，消息仍留在队列中。
    QueueRemoved,        // 队列被 IPC_RMID 删除，阻塞等待方必须退出或重建。
    PermissionDenied,    // 当前进程没有访问该队列的权限。
    ResourceUnavailable, // 内核 IPC 资源不足，通常不能靠业务重试解决。
    SystemError,         // 未被细分的系统错误。
};

const char* ipc_result_name(IpcResult result) noexcept;

struct ReceiveResult {
    IpcResult result{IpcResult::SystemError};
    std::size_t size{0};
    long message_type{0};
    std::string error;

    explicit operator bool() const noexcept {  // 只有正文已成功取出时才允许调用方读取 buffer。
        return result == IpcResult::Ok;
    }
};

}  // namespace oxmbs::ipc
