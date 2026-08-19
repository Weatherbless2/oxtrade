#pragma once

#include "oxmbs/ipc/ipc_result.h"

#include <cstddef>
#include <string>

namespace oxmbs::ipc {

struct QueueOptions {
    std::size_t max_bytes{0};  // 队列容量目标；Linux 可能因内核权限保留默认上限。
    int permissions{0600};    // 主进程和同用户 Worker 可访问，其他用户默认不可访问。
};

class SystemVMessageQueue {
public:
    SystemVMessageQueue() = default;
    ~SystemVMessageQueue();

    SystemVMessageQueue(const SystemVMessageQueue&) = delete;
    SystemVMessageQueue& operator=(const SystemVMessageQueue&) = delete;

    SystemVMessageQueue(SystemVMessageQueue&& other) noexcept;
    SystemVMessageQueue& operator=(SystemVMessageQueue&& other) noexcept;

    IpcResult create(const QueueOptions& options, std::string& error);  // 创建并记录队列 id，失败不留下内核资源。
    IpcResult send(long message_type,  // mtype 必须为正数；正文长度不包含 System V 头部。
                   const void* data,
                   std::size_t size,
                   bool non_blocking,
                   std::string& error) const;
    ReceiveResult receive(long message_type,  // 0 表示接收任意类型，否则按 System V 类型过滤。
                          void* buffer,
                          std::size_t capacity,
                          bool non_blocking) const;
    IpcResult remove(std::string& error) noexcept;  // 由队列所有者调用 IPC_RMID，重复清理视为成功。

    bool is_open() const noexcept;
    int id() const noexcept;
    std::size_t max_bytes() const noexcept;

private:
    int id_{-1};
    std::size_t max_bytes_{0};
};

}  // namespace oxmbs::ipc
