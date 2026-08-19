#pragma once

#include "oxmbs/ipc/systemv_message_queue.h"

#include <string>

namespace oxmbs::ipc {

struct IpcQueueOptions {
    QueueOptions request;
    QueueOptions reply;
};

class IpcQueueManager {
public:
    IpcQueueManager() = default;
    ~IpcQueueManager();

    IpcQueueManager(const IpcQueueManager&) = delete;
    IpcQueueManager& operator=(const IpcQueueManager&) = delete;

    IpcResult open(const IpcQueueOptions& options, std::string& error);  // 先 Request 后 Reply，后者失败会回滚前者。
    IpcResult close(std::string& error) noexcept;  // 主进程统一删除两条队列并合并清理错误。

    bool is_open() const noexcept;
    SystemVMessageQueue& request_queue() noexcept;
    const SystemVMessageQueue& request_queue() const noexcept;
    SystemVMessageQueue& reply_queue() noexcept;
    const SystemVMessageQueue& reply_queue() const noexcept;

private:
    SystemVMessageQueue request_queue_;
    SystemVMessageQueue reply_queue_;
};

}  // namespace oxmbs::ipc
