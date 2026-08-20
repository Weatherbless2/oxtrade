#include "oxmbs/ipc/ipc_queue_manager.h"

namespace oxmbs::ipc {

IpcQueueManager::~IpcQueueManager() {
    std::string ignored;
    close(ignored);  // RAII 最后防线：异常返回或调用方漏调 close() 时仍依次删除本 manager 持有的双队列。
}

IpcResult IpcQueueManager::open(const IpcQueueOptions& options, std::string& error) {
    // 双队列对业务是一个整体：只有 Request 和 Reply 都可用时 is_open() 才为 true，Worker 才应开始收发。
    if (is_open()) {
        error = "IPC queues are already open";
        return IpcResult::InvalidArgument;
    }

    // 第一步建立 Request Queue：主进程向这里发送原始请求，Worker 从这里按 mtype 接收。
    const auto request_result = request_queue_.create(options.request, error);
    if (request_result != IpcResult::Ok) {
        return request_result;
    }

    // 第二步建立 Reply Queue：Worker 把处理结果发送到这里，主进程从这里接收；两条队列方向固定且 id 不同。
    const auto reply_result = reply_queue_.create(options.reply, error);
    if (reply_result != IpcResult::Ok) {
        std::string cleanup_error;
        // Reply 创建失败时 Request 已无法构成完整通道，必须立即 IPC_RMID 回滚，避免主进程误启动 Worker
        // 或在内核中留下只有请求、没有应答能力的半初始化资源。
        request_queue_.remove(cleanup_error);
        if (!cleanup_error.empty()) {
            error += "; request queue cleanup failed: " + cleanup_error;
        }
        return reply_result;
    }

    return IpcResult::Ok;  // 只有执行到这里双队列才全部就绪，调用方可以安全启动/放行 Worker。
}

IpcResult IpcQueueManager::close(std::string& error) noexcept {
    // 关闭时两条队列彼此独立：不能因第一条删除失败而跳过第二条，否则会把 Reply Queue 泄漏到内核。
    IpcResult result = IpcResult::Ok;
    std::string request_error;
    std::string reply_error;

    // 先删除 Request 会唤醒并终止仍阻塞等请求的 Worker；随后再删除 Reply，关闭返回链路。
    const auto request_result = request_queue_.remove(request_error);
    if (request_result != IpcResult::Ok) {
        result = request_result;
    }

    const auto reply_result = reply_queue_.remove(reply_error);  // 无论 Request 结果如何，都履行 Reply 的资源清理责任。
    if (reply_result != IpcResult::Ok && result == IpcResult::Ok) {
        result = reply_result;
    }

    // 返回首个 IpcResult，同时合并两边的系统错误文本，使日志不会丢掉第二次清理的诊断信息。
    if (!request_error.empty()) {
        error += "request queue: " + request_error;
    }
    if (!reply_error.empty()) {
        if (!error.empty()) {
            error += "; ";
        }
        error += "reply queue: " + reply_error;
    }
    return result;
}

bool IpcQueueManager::is_open() const noexcept {
    return request_queue_.is_open() && reply_queue_.is_open();
}

SystemVMessageQueue& IpcQueueManager::request_queue() noexcept {
    return request_queue_;
}

const SystemVMessageQueue& IpcQueueManager::request_queue() const noexcept {
    return request_queue_;
}

SystemVMessageQueue& IpcQueueManager::reply_queue() noexcept {
    return reply_queue_;
}

const SystemVMessageQueue& IpcQueueManager::reply_queue() const noexcept {
    return reply_queue_;
}

}  // namespace oxmbs::ipc
