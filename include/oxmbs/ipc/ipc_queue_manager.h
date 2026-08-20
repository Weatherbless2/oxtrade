#pragma once

#include "oxmbs/ipc/systemv_message_queue.h"

#include <string>

namespace oxmbs::ipc {

struct IpcQueueOptions {
    QueueOptions request;  // 主进程发给 Worker 的请求通道容量和权限。
    QueueOptions reply;    // Worker 返回主进程的应答通道容量和权限。
};

class IpcQueueManager {
public:
    IpcQueueManager() = default;
    ~IpcQueueManager();

    IpcQueueManager(const IpcQueueManager&) = delete;
    IpcQueueManager& operator=(const IpcQueueManager&) = delete;

    IpcResult open(const IpcQueueOptions& options, std::string& error);  // 原子式建立 Request->Reply；第二步失败会回滚第一步。
    IpcResult close(std::string& error) noexcept;  // 主进程依次删除两条队列，即使一条失败也继续清理另一条。

    bool is_open() const noexcept;
    SystemVMessageQueue& request_queue() noexcept;
    const SystemVMessageQueue& request_queue() const noexcept;
    SystemVMessageQueue& reply_queue() noexcept;
    const SystemVMessageQueue& reply_queue() const noexcept;

private:
    SystemVMessageQueue request_queue_;  // manager 持有 Request 队列所有权，Worker 不负责删除。
    SystemVMessageQueue reply_queue_;    // manager 持有 Reply 队列所有权，析构时由 RAII 兜底释放。
};

}  // namespace oxmbs::ipc
