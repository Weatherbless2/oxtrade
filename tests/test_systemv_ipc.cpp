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
    options.request.max_bytes = 4096;
    options.reply.max_bytes = 4096;
    options.request.permissions = 0600;
    options.reply.permissions = 0600;
    return options;
}

void test_parent_child_request_reply() {
#if defined(__linux__)
    oxmbs::ipc::IpcQueueManager manager;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);
    assert(manager.is_open());
    assert(manager.request_queue().id() != manager.reply_queue().id());

    const pid_t child = ::fork();  // 用真实父子进程验证队列跨地址空间通信，而不是只测同进程调用。
    assert(child >= 0);
    if (child == 0) {
        char request[64]{};
        const auto received = manager.request_queue().receive(10, request, sizeof(request), false);  // Worker 阻塞等待请求。
        if (!received || std::string(request, received.size) != "query-request") {
            _exit(11);
        }

        const std::string response = "query-response";
        std::string child_error;
        if (manager.reply_queue().send(20, response.data(), response.size(), false, child_error) !=
            oxmbs::ipc::IpcResult::Ok) {
            _exit(12);
        }
        _exit(0);
    }

    const std::string request = "query-request";
    assert(manager.request_queue().send(10, request.data(), request.size(), false, error) ==
           oxmbs::ipc::IpcResult::Ok);

    char response[64]{};
    const auto received = manager.reply_queue().receive(20, response, sizeof(response), false);  // 主进程阻塞收取 Worker 应答。
    assert(received);
    assert(received.message_type == 20);
    assert(std::string(response, received.size) == "query-response");

    int status = 0;
    assert(::waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);
    assert(!manager.is_open());
#endif
}

void test_non_blocking_empty_receive() {
    oxmbs::ipc::IpcQueueManager manager;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);

    char buffer[16]{};
    const auto received = manager.request_queue().receive(1, buffer, sizeof(buffer), true);  // 空队列不能阻塞测试进程。
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

    char buffer[4]{};
    const auto received = manager.request_queue().receive(1, buffer, sizeof(buffer), true);  // 缓冲区不足时消息应保留并返回 E2BIG 映射。
    assert(!received);
    assert(received.result == oxmbs::ipc::IpcResult::MessageTooLarge);
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);
}

void test_queue_removal_wakes_blocked_receiver() {
#if defined(__linux__)
    oxmbs::ipc::IpcQueueManager manager;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);

    const pid_t child = ::fork();  // 子进程进入阻塞 msgrcv，父进程删除队列验证唤醒路径。
    assert(child >= 0);
    if (child == 0) {
        char buffer[16]{};
        const auto received = manager.request_queue().receive(1, buffer, sizeof(buffer), false);
        _exit(received.result == oxmbs::ipc::IpcResult::QueueRemoved ? 0 : 13);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    assert(manager.request_queue().remove(error) == oxmbs::ipc::IpcResult::Ok);  // IPC_RMID 应让子进程收到 QueueRemoved。

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
    options.reply.max_bytes = 0;
    assert(manager.open(options, error) == oxmbs::ipc::IpcResult::InvalidArgument);  // Reply 参数非法时验证 Request 自动回滚。
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
