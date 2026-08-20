#include "oxmbs/ipc/ipc_queue_manager.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#if defined(__linux__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

oxmbs::ipc::IpcQueueOptions queue_options() {
    oxmbs::ipc::IpcQueueOptions options;
    options.request.max_bytes = 4096;  // 测试正文很小，4 KiB 足以覆盖收发且不依赖提高系统 MSGMNB 上限的权限。
    options.reply.max_bytes = 4096;  // 两个方向使用相同容量，测试重点放在通道方向与资源生命周期。
    options.request.permissions = 0600;  // fork 子进程继承创建者身份和 queue id，无需向其他系统用户开放权限。
    options.reply.permissions = 0600;
    return options;
}

void test_parent_child_request_reply() {
#if defined(__linux__)
    oxmbs::ipc::IpcQueueManager manager;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);
    assert(manager.is_open());
    assert(manager.request_queue().id() != manager.reply_queue().id());  // 证明 Request/Reply 是两份独立内核资源，不是同队列别名。

    // fork 后父子进程拥有不同地址空间但继承相同的两个 queue id，可真实验证 System V IPC，
    // 也模拟主进程与 Worker 的 Request -> 处理 -> Reply 往返链路，而不是只验证同进程内函数调用。
    const pid_t child = ::fork();
    assert(child >= 0);
    if (child == 0) {
        char request[64]{};
        // 子进程模拟 Worker：在 Request Queue 按 mtype=10 阻塞，父进程发送后才被内核唤醒。
        const auto received = manager.request_queue().receive(10, request, sizeof(request), false);
        if (!received || std::string(request, received.size) != "query-request") {
            _exit(11);  // 11 表示请求接收失败，或按实际字节长度还原出的正文不一致。
        }

        const std::string response = "query-response";
        std::string child_error;
        if (manager.reply_queue().send(20, response.data(), response.size(), false, child_error) !=
            oxmbs::ipc::IpcResult::Ok) {
            _exit(12);  // 12 表示 Worker 已收到请求，但向 Reply Queue 发送应答失败。
        }
        _exit(0);
    }

    const std::string request = "query-request";
    // 主进程把 request.size() 个原始字节和路由类型 10 发入 Request Queue；不发送 std::string 末尾的 '\0'。
    assert(manager.request_queue().send(10, request.data(), request.size(), false, error) ==
           oxmbs::ipc::IpcResult::Ok);

    char response[64]{};
    // 主进程在 Reply Queue 按 mtype=20 阻塞，串起“父进程发请求 -> Worker 收请求 -> Worker 发应答 -> 父进程收应答”。
    const auto received = manager.reply_queue().receive(20, response, sizeof(response), false);
    assert(received);
    assert(received.message_type == 20);  // 校验 mtype 经过内核路由和封装解析后保持不变。
    // 只按 received.size 构造字符串并比较，既校验正文字节也校验精确长度，不依赖缓冲区清零或隐式 '\0'。
    assert(std::string(response, received.size) == "query-response");

    int status = 0;
    assert(::waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);  // 非 0 时结合 11/12 可定位失败发生在 Worker 收请求还是发应答。
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);
    assert(!manager.is_open());
#endif
}

void test_non_blocking_empty_receive() {
    oxmbs::ipc::IpcQueueManager manager;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);

    char buffer[16]{};
    // 空队列上显式启用 IPC_NOWAIT，测试必须立即返回；若实现误入阻塞，测试会在这里挂住而不是得到断言结果。
    const auto received = manager.request_queue().receive(1, buffer, sizeof(buffer), true);
    assert(!received);
    assert(received.result == oxmbs::ipc::IpcResult::WouldBlock);
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);
}

void test_receive_buffer_too_small() {
    oxmbs::ipc::IpcQueueManager manager;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);

    const std::string payload = "payload-too-large-for-buffer";
    assert(manager.request_queue().send(1, payload.data(), payload.size(), false, error) ==
           oxmbs::ipc::IpcResult::Ok);

    // 人为只给 4 字节，而队首正文更长，稳定制造 msgrcv 的 E2BIG；未使用 MSG_NOERROR，因此消息不能被截断取走。
    char buffer[4]{};
    const auto received = manager.request_queue().receive(1, buffer, sizeof(buffer), true);
    assert(!received);
    assert(received.result == oxmbs::ipc::IpcResult::MessageTooLarge);
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);
}

void test_queue_removal_wakes_blocked_receiver() {
#if defined(__linux__)
    oxmbs::ipc::IpcQueueManager manager;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);

    // 真实子进程进入阻塞 msgrcv，父进程从另一执行流调用 IPC_RMID，验证内核唤醒而不是普通函数返回。
    const pid_t child = ::fork();
    assert(child >= 0);
    if (child == 0) {
        char buffer[16]{};
        const auto received = manager.request_queue().receive(1, buffer, sizeof(buffer), false);
        _exit(received.result == oxmbs::ipc::IpcResult::QueueRemoved ? 0 : 13);  // 13 表示删除虽发生，但未正确映射 EIDRM。
    }

    // 给子进程时间进入内核阻塞点，再删除 Request Queue；IPC_RMID 应使其 msgrcv 返回 EIDRM -> QueueRemoved。
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    assert(manager.request_queue().remove(error) == oxmbs::ipc::IpcResult::Ok);

    int status = 0;
    assert(::waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(manager.reply_queue().remove(error) == oxmbs::ipc::IpcResult::Ok);
#endif
}

void test_manager_rolls_back_request_on_reply_failure() {
    oxmbs::ipc::IpcQueueManager manager;
    std::string error;
    oxmbs::ipc::IpcQueueOptions options = queue_options();
    options.reply.max_bytes = 0;  // Request 参数保持有效、只破坏 Reply，确保失败发生在第二步而不是第一步。
    assert(manager.open(options, error) == oxmbs::ipc::IpcResult::InvalidArgument);
    // 两边都关闭证明 manager 在 Reply 创建失败后删除了已成功创建的 Request，没有留下半初始化双队列。
    assert(!manager.request_queue().is_open());
    assert(!manager.reply_queue().is_open());
}

}  // namespace

int main() {
    test_parent_child_request_reply();
    test_non_blocking_empty_receive();
    test_receive_buffer_too_small();
    test_queue_removal_wakes_blocked_receiver();
    test_manager_rolls_back_request_on_reply_failure();
    return 0;
}
