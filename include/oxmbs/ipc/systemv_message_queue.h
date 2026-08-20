#pragma once

#include "oxmbs/ipc/ipc_result.h"

#include <cstddef>
#include <string>

namespace oxmbs::ipc {

struct QueueOptions {
    std::size_t max_bytes{0};  // 整条队列允许缓存的正文总容量目标，不是单条消息的正文长度。
    int permissions{0600};    // System V 权限位；0600 使创建者可读写，其他用户不能访问。
};

class SystemVMessageQueue {
public:
    SystemVMessageQueue() = default;
    ~SystemVMessageQueue();

    SystemVMessageQueue(const SystemVMessageQueue&) = delete;
    SystemVMessageQueue& operator=(const SystemVMessageQueue&) = delete;

    SystemVMessageQueue(SystemVMessageQueue&& other) noexcept;
    SystemVMessageQueue& operator=(SystemVMessageQueue&& other) noexcept;

    IpcResult create(const QueueOptions& options, std::string& error);  // 由主进程申请内核队列，属性完成后才取得本对象所有权。
    IpcResult send(long message_type,  // mtype 是路由标签且必须为正数；size 只表示正文，不含 long 头部。
                   const void* data,
                   std::size_t size,
                   bool non_blocking,
                   std::string& error) const;
    ReceiveResult receive(long message_type,  // 0 表示接收任意类型，正数表示按 mtype 过滤；调用方提供正文 buffer。
                          void* buffer,
                          std::size_t capacity,
                          bool non_blocking) const;
    IpcResult remove(std::string& error) noexcept;  // 所有者调用 IPC_RMID，先失效本地 id 再删内核对象，重复清理视为成功。

    bool is_open() const noexcept;
    int id() const noexcept;
    std::size_t max_bytes() const noexcept;

private:
    int id_{-1};  // 内核返回的 queue id；-1 表示本对象不拥有可用队列。
    std::size_t max_bytes_{0};  // 创建后内核实际接受的 msg_qbytes，失败/移交后归零。
};

}  // namespace oxmbs::ipc
