#include "oxmbs/ipc/systemv_message_queue.h"

#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <sys/ipc.h>
#include <sys/msg.h>
#endif

namespace oxmbs::ipc {
namespace {

IpcResult map_errno(int error_number) noexcept {
    // 把 Linux errno 转为业务层稳定状态，调用方不需要依赖平台宏判断每个错误。
    switch (error_number) {
    case EINTR:
        return IpcResult::Interrupted;
    case EAGAIN:
    case ENOMSG:
        return IpcResult::WouldBlock;
    case E2BIG:
        return IpcResult::MessageTooLarge;
    case EIDRM:
        return IpcResult::QueueRemoved;
    case EACCES:
        return IpcResult::PermissionDenied;
    case ENOSPC:
    case ENOMEM:
        return IpcResult::ResourceUnavailable;
    case EINVAL:
        return IpcResult::InvalidArgument;
    default:
        return IpcResult::SystemError;
    }
}

std::string system_error(const char* operation, int error_number) {
    return std::string(operation) + " failed: " + std::strerror(error_number);
}

}  // namespace

const char* ipc_result_name(IpcResult result) noexcept {
    switch (result) {
    case IpcResult::Ok:
        return "Ok";
    case IpcResult::InvalidArgument:
        return "InvalidArgument";
    case IpcResult::NotOpen:
        return "NotOpen";
    case IpcResult::Interrupted:
        return "Interrupted";
    case IpcResult::WouldBlock:
        return "WouldBlock";
    case IpcResult::MessageTooLarge:
        return "MessageTooLarge";
    case IpcResult::QueueRemoved:
        return "QueueRemoved";
    case IpcResult::PermissionDenied:
        return "PermissionDenied";
    case IpcResult::ResourceUnavailable:
        return "ResourceUnavailable";
    case IpcResult::SystemError:
        return "SystemError";
    }
    return "Unknown";
}

SystemVMessageQueue::~SystemVMessageQueue() {
    std::string ignored;
    remove(ignored);  // 队列对象拥有内核资源；析构时兜底删除，正常流程仍由 manager 显式 close。
}

SystemVMessageQueue::SystemVMessageQueue(SystemVMessageQueue&& other) noexcept
    : id_(std::exchange(other.id_, -1)), max_bytes_(std::exchange(other.max_bytes_, 0)) {}

SystemVMessageQueue& SystemVMessageQueue::operator=(SystemVMessageQueue&& other) noexcept {
    if (this != &other) {
        std::string ignored;
        remove(ignored);
        id_ = std::exchange(other.id_, -1);
        max_bytes_ = std::exchange(other.max_bytes_, 0);
    }
    return *this;
}

IpcResult SystemVMessageQueue::create(const QueueOptions& options, std::string& error) {
    if (id_ >= 0 || options.max_bytes == 0 || options.permissions < 0 || options.permissions > 0777) {
        error = "queue is already open or options are invalid";
        return IpcResult::InvalidArgument;
    }

#if defined(__linux__)
    const int queue_id = ::msgget(IPC_PRIVATE, IPC_CREAT | options.permissions);  // IPC_PRIVATE 保证每次创建独立队列。
    if (queue_id < 0) {
        error = system_error("msgget", errno);
        return map_errno(errno);
    }

    struct msqid_ds status {};  // 先读取内核默认属性，再尝试设置目标容量。
    if (::msgctl(queue_id, IPC_STAT, &status) < 0) {
        error = system_error("msgctl(IPC_STAT)", errno);
        ::msgctl(queue_id, IPC_RMID, nullptr);
        return map_errno(errno);
    }

    // Linux 默认 MSGMNB 可能小于业务需要；尽量设置目标容量，失败时保留内核默认值。
    status.msg_qbytes = options.max_bytes;
    if (::msgctl(queue_id, IPC_SET, &status) < 0 && errno != EPERM) {
        error = system_error("msgctl(IPC_SET)", errno);
        ::msgctl(queue_id, IPC_RMID, nullptr);
        return map_errno(errno);
    }

    id_ = queue_id;  // 只有属性初始化完成后才对外暴露有效 id。
    max_bytes_ = status.msg_qbytes;
    return IpcResult::Ok;
#else
    (void)options;
    error = "System V message queues are only implemented for Linux in this work order";
    return IpcResult::SystemError;
#endif
}

IpcResult SystemVMessageQueue::send(long message_type,
                                    const void* data,
                                    std::size_t size,
                                    bool non_blocking,
                                    std::string& error) const {
    if (id_ < 0 || data == nullptr || size == 0 || message_type <= 0) {
        error = "queue is not open or message arguments are invalid";
        return id_ < 0 ? IpcResult::NotOpen : IpcResult::InvalidArgument;
    }

#if defined(__linux__)
    // System V 要求内存首部为 long mtype，但 msgsz 只计算正文；封装层隐藏这个 ABI 细节。
    std::vector<unsigned char> message(sizeof(long) + size);
    std::memcpy(message.data(), &message_type, sizeof(message_type));
    std::memcpy(message.data() + sizeof(message_type), data, size);
    const auto flags = non_blocking ? IPC_NOWAIT : 0;
    if (::msgsnd(id_, message.data(), size, flags) < 0) {  // 阻塞模式下队列满会等待，非阻塞模式返回 WouldBlock。
        error = system_error("msgsnd", errno);
        return map_errno(errno);
    }
    return IpcResult::Ok;
#else
    (void)message_type;
    (void)data;
    (void)size;
    (void)non_blocking;
    error = "System V message queues are only implemented for Linux in this work order";
    return IpcResult::SystemError;
#endif
}

ReceiveResult SystemVMessageQueue::receive(long message_type,
                                           void* buffer,
                                           std::size_t capacity,
                                           bool non_blocking) const {
    ReceiveResult result;
    if (id_ < 0) {
        result.result = IpcResult::NotOpen;
        result.error = "queue is not open";
        return result;
    }
    if (buffer == nullptr || capacity == 0 || message_type < 0) {
        result.result = IpcResult::InvalidArgument;
        result.error = "message arguments are invalid";
        return result;
    }

#if defined(__linux__)
    std::vector<unsigned char> message(sizeof(long) + capacity);
    const auto flags = non_blocking ? IPC_NOWAIT : 0;
    const auto received = ::msgrcv(id_, message.data(), capacity, message_type, flags);  // 空队列阻塞等待，删除队列返回 EIDRM。
    if (received < 0) {
        result.result = map_errno(errno);
        result.error = system_error("msgrcv", errno);
        return result;
    }
    result.result = IpcResult::Ok;
    result.size = static_cast<std::size_t>(received);
    std::memcpy(&result.message_type, message.data(), sizeof(result.message_type));
    std::memcpy(buffer, message.data() + sizeof(long), result.size);  // 只把正文复制给业务，避免暴露 mtype 内存布局。
    return result;
#else
    (void)message_type;
    (void)buffer;
    (void)capacity;
    (void)non_blocking;
    result.result = IpcResult::SystemError;
    result.error = "System V message queues are only implemented for Linux in this work order";
    return result;
#endif
}

IpcResult SystemVMessageQueue::remove(std::string& error) noexcept {
    if (id_ < 0) {
        max_bytes_ = 0;
        return IpcResult::Ok;
    }

#if defined(__linux__)
    const int queue_id = id_;
    id_ = -1;
    max_bytes_ = 0;
    if (::msgctl(queue_id, IPC_RMID, nullptr) < 0) {  // 删除会同时唤醒正在 msgrcv 等待的 Worker。
        if (errno == EIDRM || errno == EINVAL) {
            return IpcResult::Ok;
        }
        error = system_error("msgctl(IPC_RMID)", errno);
        return map_errno(errno);
    }
    return IpcResult::Ok;
#else
    id_ = -1;
    max_bytes_ = 0;
    error = "System V message queues are only implemented for Linux in this work order";
    return IpcResult::SystemError;
#endif
}

bool SystemVMessageQueue::is_open() const noexcept {
    return id_ >= 0;
}

int SystemVMessageQueue::id() const noexcept {
    return id_;
}

std::size_t SystemVMessageQueue::max_bytes() const noexcept {
    return max_bytes_;
}

}  // namespace oxmbs::ipc
