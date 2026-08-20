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
    // msgsnd/msgrcv/msgctl 失败时只通过 errno 描述原因；这里统一转换成稳定的 IpcResult，
    // 让 manager、Worker 和测试按“可重试、队列已删除、权限不足”等业务语义处理，而不必各自判断 Linux 宏。
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
    return std::string(operation) + " failed: " + std::strerror(error_number);  // 保留调用名和 strerror 文本，便于日志定位。
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
    remove(ignored);  // RAII 兜底：对象销毁时删除仍由它拥有的内核队列，避免异常路径留下 IPC 资源。
}

SystemVMessageQueue::SystemVMessageQueue(SystemVMessageQueue&& other) noexcept
    : id_(std::exchange(other.id_, -1)), max_bytes_(std::exchange(other.max_bytes_, 0)) {
    // 所有权从 other 转给 this；other 被清空后，析构时不会对同一个 queue id 二次 IPC_RMID。
}

SystemVMessageQueue& SystemVMessageQueue::operator=(SystemVMessageQueue&& other) noexcept {
    if (this != &other) {
        std::string ignored;
        remove(ignored);  // 赋值前先释放 this 原有资源，防止覆盖 id_ 导致内核队列泄漏。
        id_ = std::exchange(other.id_, -1);  // 接管 queue id，同时让 other 失去删除责任。
        max_bytes_ = std::exchange(other.max_bytes_, 0);  // 一并转移内核最终容量记录。
    }
    return *this;
}

IpcResult SystemVMessageQueue::create(const QueueOptions& options, std::string& error) {
    // create() 是单队列的创建事务：只有“新建队列、读取属性、尝试设置容量”全部走完后才写入 id_；
    // 中途失败会删除临时 queue_id，因此调用方看到失败时，本对象仍是未打开状态，也不会留下无主内核队列。
    if (id_ >= 0 || options.max_bytes == 0 || options.permissions < 0 || options.permissions > 0777) {
        error = "queue is already open or options are invalid";
        return IpcResult::InvalidArgument;
    }

#if defined(__linux__)
    // IPC_PRIVATE 不是查找一个共享名字，而是要求内核每次分配全新的队列；父进程后续把得到的 queue id
    // 交给/继承给 Worker。IPC_CREAT 与 permissions 决定新队列的创建及访问权限，默认 0600 仅允许创建者读写。
    const int queue_id = ::msgget(IPC_PRIVATE, IPC_CREAT | options.permissions);
    if (queue_id < 0) {
        error = system_error("msgget", errno);
        return map_errno(errno);
    }

    // IPC_STAT 读取内核刚创建队列的完整属性。必须先取回 msqid_ds 再只修改 msg_qbytes，
    // 避免用未初始化结构执行 IPC_SET 时意外覆盖权限、所有者等其他字段。
    struct msqid_ds status {};
    if (::msgctl(queue_id, IPC_STAT, &status) < 0) {
        error = system_error("msgctl(IPC_STAT)", errno);
        ::msgctl(queue_id, IPC_RMID, nullptr);  // 属性读取失败，回滚刚创建且尚未发布给对象的临时队列。
        return map_errno(errno);
    }

    // msg_qbytes 是“队列中所有正文的累计上限”，不是单条消息长度。Linux 允许普通用户在权限范围内调整它；
    // 提高到系统限制以上通常返回 EPERM，此时队列本身仍然可用，所以保留内核默认容量并继续启动。
    status.msg_qbytes = options.max_bytes;
    if (::msgctl(queue_id, IPC_SET, &status) < 0 && errno != EPERM) {
        error = system_error("msgctl(IPC_SET)", errno);
        ::msgctl(queue_id, IPC_RMID, nullptr);  // 非权限类初始化失败必须回滚，不能暴露只完成一半配置的队列。
        return map_errno(errno);
    }

    id_ = queue_id;  // 到达这里才发布 queue id，建立“id_ >= 0 就由本对象负责 IPC_RMID”的所有权约定。
    // 正常 IPC_SET 成功时记录目标容量；若因 EPERM 保留内核默认值，此字段仍保存请求值，不能用它替代 IPC_STAT 查询实际值。
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
    // id_ 必须指向本对象持有的队列；本工单不定义“空正文”协议；System V 规定发送消息的 mtype 必须大于 0。
    if (id_ < 0 || data == nullptr || size == 0 || message_type <= 0) {
        error = "queue is not open or message arguments are invalid";
        return id_ < 0 ? IpcResult::NotOpen : IpcResult::InvalidArgument;
    }

#if defined(__linux__)
    // System V ABI 要求传给 msgsnd 的首地址按 [long mtype][正文 size 字节] 排列。
    // data 是未经解析的原始字节，可包含 '\0'，封装不会添加字符串终止符、序列化字段或改变正文长度。
    std::vector<unsigned char> message(sizeof(long) + size);
    std::memcpy(message.data(), &message_type, sizeof(message_type));  // 写入仅供内核筛选消息的 mtype 头。
    std::memcpy(message.data() + sizeof(message_type), data, size);  // 紧随其后逐字节复制业务正文，不做文本转换。
    const auto flags = non_blocking ? IPC_NOWAIT : 0;  // true 明确禁止线程在“队列已满”处等待。
    // msgsz 按 System V 约定只传正文 size，不能把 sizeof(long) 算进去，否则接收端会多看到一个头部长度。
    // 阻塞模式下队列空间不足会停在内核中等待；IPC_NOWAIT 模式立即以 EAGAIN 返回并映射为 WouldBlock。
    if (::msgsnd(id_, message.data(), size, flags) < 0) {
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
    // 先区分“对象未持有队列”和“调用参数非法”，上层据此决定先 open() 还是修正 buffer/type。
    if (id_ < 0) {
        result.result = IpcResult::NotOpen;
        result.error = "queue is not open";
        return result;
    }
    // capacity 只表示调用方正文 buffer 的可写字节数；mtype 由封装自己的临时内存承载，不占用该容量。
    if (buffer == nullptr || capacity == 0 || message_type < 0) {
        result.result = IpcResult::InvalidArgument;
        result.error = "message arguments are invalid";
        return result;
    }

#if defined(__linux__)
    // msgrcv 同样要求目标内存是 [long mtype][正文]，所以临时空间比调用方正文容量多 sizeof(long)。
    std::vector<unsigned char> message(sizeof(long) + capacity);
    const auto flags = non_blocking ? IPC_NOWAIT : 0;  // true 时空队列立即返回 ENOMSG，避免当前线程挂起。
    // message_type == 0 接收队列中的第一条消息；正数只接收相同 mtype。msgsz 仍只传正文 capacity。
    // 阻塞模式在没有匹配消息时睡眠，直到消息到达、信号中断，或所有者 IPC_RMID 删除队列并以 EIDRM 唤醒它。
    const auto received = ::msgrcv(id_, message.data(), capacity, message_type, flags);
    if (received < 0) {
        result.result = map_errno(errno);
        result.error = system_error("msgrcv", errno);
        return result;
    }
    result.result = IpcResult::Ok;
    result.size = static_cast<std::size_t>(received);  // msgrcv 返回实际正文长度，不包含 long mtype，也不包含终止符。
    std::memcpy(&result.message_type, message.data(), sizeof(result.message_type));  // 返回内核实际取出的消息类型供调用方核对。
    // 只把实际收到的正文复制给业务 buffer；剩余 capacity 不写入，且不追加 '\0'，二进制和文本均按 result.size 取用。
    std::memcpy(buffer, message.data() + sizeof(long), result.size);
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
    // remove() 可重复调用：未持有 queue id 直接成功，使显式 close() 与析构 RAII 兜底可以安全叠加。
    if (id_ < 0) {
        max_bytes_ = 0;
        return IpcResult::Ok;
    }

#if defined(__linux__)
    const int queue_id = id_;  // 先保存本次唯一需要删除的内核 id。
    id_ = -1;  // 在系统调用前撤销本对象所有权；即使删除报错，析构也不会误删后来复用同一数字的新队列。
    max_bytes_ = 0;
    // IPC_RMID 立即把队列标记为删除，并唤醒阻塞在 msgsnd/msgrcv 的进程；其系统调用以 EIDRM 失败，
    // map_errno() 再转换成 QueueRemoved，Worker 因而能把“正常关闭”与普通接收错误区分开并退出循环。
    if (::msgctl(queue_id, IPC_RMID, nullptr) < 0) {
        if (errno == EIDRM || errno == EINVAL) {
            // 其他路径已经删除队列，或该 id 已不再有效时，目标状态同样是“本对象不再拥有资源”，按幂等成功处理。
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
