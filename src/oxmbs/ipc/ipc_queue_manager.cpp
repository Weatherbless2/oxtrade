#include "oxmbs/ipc/ipc_queue_manager.h"

namespace oxmbs::ipc {

IpcQueueManager::~IpcQueueManager() {
    std::string ignored;
    close(ignored);
}

IpcResult IpcQueueManager::open(const IpcQueueOptions& options, std::string& error) {
    if (is_open()) {
        error = "IPC queues are already open";
        return IpcResult::InvalidArgument;
    }

    const auto request_result = request_queue_.create(options.request, error);
    if (request_result != IpcResult::Ok) {
        return request_result;
    }

    const auto reply_result = reply_queue_.create(options.reply, error);
    if (reply_result != IpcResult::Ok) {
        std::string cleanup_error;
        request_queue_.remove(cleanup_error);
        if (!cleanup_error.empty()) {
            error += "; request queue cleanup failed: " + cleanup_error;
        }
        return reply_result;
    }

    return IpcResult::Ok;
}

IpcResult IpcQueueManager::close(std::string& error) noexcept {
    IpcResult result = IpcResult::Ok;
    std::string request_error;
    std::string reply_error;

    const auto request_result = request_queue_.remove(request_error);
    if (request_result != IpcResult::Ok) {
        result = request_result;
    }

    const auto reply_result = reply_queue_.remove(reply_error);
    if (reply_result != IpcResult::Ok && result == IpcResult::Ok) {
        result = reply_result;
    }

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
