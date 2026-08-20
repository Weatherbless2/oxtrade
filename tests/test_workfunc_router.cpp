#include "oxmbs/ipc/ipc_queue_manager.h"
#include "oxmbs/ipc/task_correlation.h"
#include "oxmbs/ipc/task_protocol.h"
#include "oxmbs/routing/message_route_policy.h"
#include "oxmbs/routing/mock_pack_message.h"
#include "oxmbs/routing/workfunc_router.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::uint32_t kMockLoginMessageType = 9001;  // 临时模拟“客户端登录”，仅用于 #6 路由验收，不能当作真实 HP-Socket 枚举。
constexpr std::uint32_t kMockUserQueryMessageType = 9002;  // 临时模拟“查询用户资料”，应进入低频 Worker 队列。
constexpr std::uint32_t kMockOrderMessageType = 5001;  // 临时模拟下单，高频链路必须直通且不能产生 IPC 任务。
constexpr std::uint32_t kMockMarketMessageType = 5002;  // 临时模拟行情，高频链路必须直通且不能触碰 Request Queue。
constexpr long kRequestQueueMessageType = 7;  // System V mtype 仅供内核筛选，与上面的业务消息类型刻意不同。

oxmbs::ipc::IpcQueueOptions queue_options(std::size_t max_bytes = 4096) {
    oxmbs::ipc::IpcQueueOptions options;
    options.request.max_bytes = max_bytes;
    options.reply.max_bytes = max_bytes;
    options.request.permissions = 0600;
    options.reply.permissions = 0600;
    return options;
}

oxmbs::routing::MessageRoutePolicy mock_route_policy() {
    return oxmbs::routing::MessageRoutePolicy(
        std::unordered_set<std::uint32_t>{kMockLoginMessageType, kMockUserQueryMessageType});  // 只有模拟登录/用户查询进 IPC，其余默认走原 WorkFunc。
}

oxmbs::routing::WorkFuncRouterOptions router_options(std::size_t max_frame_bytes = 4096) {
    oxmbs::routing::WorkFuncRouterOptions options;
    options.protocol_limits.max_frame_bytes = max_frame_bytes;
    options.query_task_timeout = std::chrono::milliseconds{250};  // 测试只验证注册/编码/入队，不依赖真实等待到超时。
    options.request_message_type = kRequestQueueMessageType;
    return options;
}

oxmbs::routing::MockPackMessage make_message(oxmbs::ipc::SessionId session_id,
                                              std::uint32_t msg_type,
                                              std::uint32_t func_id,
                                              const std::vector<std::uint8_t>& payload) {
    return oxmbs::routing::MockPackMessage{session_id, msg_type, func_id, payload.data(), payload.size()};
}

void test_mock_login_is_copied_encoded_and_enqueued() {
    // 模拟客户端登录用户数据：真实 PackMessage 未到位前，JSON 只作为不透明二进制 payload，路由层不解析用户名或口令。
    std::vector<std::uint8_t> login_payload{'{', '"', 'u', 's', 'e', 'r', '"', ':', '"', 'a', 'l', 'i', 'c', 'e', '"', '}'};
    const auto expected_payload = login_payload;
    oxmbs::ipc::IpcQueueManager manager;
    oxmbs::ipc::TaskCorrelationRegistry registry;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);

    oxmbs::routing::WorkFuncRouter router(manager, registry, mock_route_policy(), router_options());
    const auto message = make_message(101, kMockLoginMessageType, 41, login_payload);
    const auto routed = router.route(message);
    assert(routed.status == oxmbs::routing::WorkFuncRouteStatus::ForwardedToWorker);
    assert(routed.task_seq != 0);
    assert(registry.pending_count() == 1);

    // route() 返回后改写原始上游 buffer，若路由器错误保存借用指针，Worker 收到的任务会被改坏。
    login_payload.assign(login_payload.size(), static_cast<std::uint8_t>('X'));
    std::vector<std::uint8_t> frame(4096);
    const auto received = manager.request_queue().receive(kRequestQueueMessageType, frame.data(), frame.size(), true);
    assert(received);
    assert(received.message_type == kRequestQueueMessageType);
    const auto decoded = oxmbs::ipc::decode_task(frame.data(), received.size);
    assert(decoded);
    assert(decoded.message.task_seq == routed.task_seq);
    assert(decoded.message.session_id == 101);
    assert(decoded.message.msg_type == kMockLoginMessageType);
    assert(decoded.message.func_id == 41);
    assert(decoded.message.payload == expected_payload);  // 证明 Request Queue 拿到的是 route() 内部深拷贝的稳定字节，不是已被改写的输入数组。

    assert(registry.cancel_request(decoded.message.task_seq, decoded.message.session_id, decoded.message.deadline_unix_ms) ==
           oxmbs::ipc::TaskCorrelationStatus::Cancelled);  // 本工单没有 Worker/Reply，测试取走任务后显式清理 pending_。
    assert(registry.pending_count() == 0);
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);
}

void test_mock_user_query_enters_worker_queue() {
    // 第二个低频样例使用模拟“用户资料查询”，证明白名单是可注入策略，不依赖某个特定登录消息的硬编码分支。
    const std::vector<std::uint8_t> query_payload{'u', 's', 'e', 'r', '-', '1', '0', '0', '8'};
    oxmbs::ipc::IpcQueueManager manager;
    oxmbs::ipc::TaskCorrelationRegistry registry;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);
    oxmbs::routing::WorkFuncRouter router(manager, registry, mock_route_policy(), router_options());

    const auto routed = router.route(make_message(202, kMockUserQueryMessageType, 42, query_payload));
    assert(routed.forwarded());
    std::vector<std::uint8_t> frame(4096);
    const auto received = manager.request_queue().receive(kRequestQueueMessageType, frame.data(), frame.size(), true);
    assert(received);
    const auto decoded = oxmbs::ipc::decode_task(frame.data(), received.size);
    assert(decoded && decoded.message.msg_type == kMockUserQueryMessageType);
    assert(decoded.message.payload == query_payload);
    assert(registry.cancel_request(decoded.message.task_seq, decoded.message.session_id, decoded.message.deadline_unix_ms) ==
           oxmbs::ipc::TaskCorrelationStatus::Cancelled);
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);
}

void test_high_frequency_messages_never_enter_ipc() {
    // 下单和行情只是 Mock 类型，但断言的不是类型数字，而是“白名单外不会读取 payload、不会登记、不会进入 Request Queue”的隔离语义。
    oxmbs::ipc::IpcQueueManager manager;
    oxmbs::ipc::TaskCorrelationRegistry registry;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);
    oxmbs::routing::WorkFuncRouter router(manager, registry, mock_route_policy(), router_options());

    const oxmbs::routing::MockPackMessage order{kMockOrderMessageType == 0 ? 0 : 301, kMockOrderMessageType, 0, nullptr, 0};
    const oxmbs::routing::MockPackMessage market{302, kMockMarketMessageType, 0, nullptr, 0};
    assert(router.route(order).status == oxmbs::routing::WorkFuncRouteStatus::HighFrequencyPassthrough);
    assert(router.route(market).status == oxmbs::routing::WorkFuncRouteStatus::HighFrequencyPassthrough);
    assert(router.forward_to_worker_process_pool(order).status ==
           oxmbs::routing::WorkFuncRouteStatus::HighFrequencyPassthrough);  // 误用底层转发入口也必须受同一白名单保护。
    assert(registry.pending_count() == 0);  // 即使高频消息没有 payload，也不能被路由器当作低频非法包处理或登记。

    std::vector<std::uint8_t> frame(4096);
    const auto received = manager.request_queue().receive(kRequestQueueMessageType, frame.data(), frame.size(), true);
    assert(!received && received.result == oxmbs::ipc::IpcResult::WouldBlock);  // 队列为空，证明高频没有被错误转发。
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);
}

void test_invalid_low_frequency_message_is_rejected_before_registration() {
    const std::vector<std::uint8_t> payload{'x'};
    oxmbs::ipc::IpcQueueManager manager;
    oxmbs::ipc::TaskCorrelationRegistry registry;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);
    oxmbs::routing::WorkFuncRouter router(manager, registry, mock_route_policy(), router_options());

    const auto zero_session = router.route(make_message(0, kMockLoginMessageType, 1, payload));
    assert(zero_session.status == oxmbs::routing::WorkFuncRouteStatus::InvalidMessage);
    const oxmbs::routing::MockPackMessage null_payload{401, kMockUserQueryMessageType, 1, nullptr, 0};
    assert(router.route(null_payload).status == oxmbs::routing::WorkFuncRouteStatus::InvalidMessage);
    assert(registry.pending_count() == 0);  // 所有非法输入都在 register_request() 之前失败，不会占用超时表。
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);
}

void test_queue_full_is_non_blocking_and_rolls_back_second_task() {
    // 64 字节队列只能容纳一条 57 字节最小 Task 帧；第二次 IPC_NOWAIT 发送必须立刻 EAGAIN -> WouldBlock。
    const std::vector<std::uint8_t> payload{'q'};
    oxmbs::ipc::IpcQueueManager manager;
    oxmbs::ipc::TaskCorrelationRegistry registry;
    std::string error;
    assert(manager.open(queue_options(64), error) == oxmbs::ipc::IpcResult::Ok);
    oxmbs::routing::WorkFuncRouter router(manager, registry, mock_route_policy(), router_options(64));

    const auto first = router.route(make_message(501, kMockLoginMessageType, 1, payload));
    assert(first.forwarded());
    assert(registry.pending_count() == 1);
    const auto second = router.route(make_message(502, kMockUserQueryMessageType, 1, payload));
    assert(second.status == oxmbs::routing::WorkFuncRouteStatus::QueueSendFailed);
    assert(second.ipc_result == oxmbs::ipc::IpcResult::WouldBlock);  // WorkFunc 得到立即失败结果，不会等待第一个任务被 Worker 取走。
    assert(second.rollback_status == oxmbs::ipc::TaskCorrelationStatus::Cancelled);
    assert(registry.pending_count() == 1);  // 只保留已真实进入队列的 first；second 的 pending_ 已被回滚。

    std::vector<std::uint8_t> frame(64);
    const auto received = manager.request_queue().receive(kRequestQueueMessageType, frame.data(), frame.size(), true);
    assert(received);
    const auto decoded = oxmbs::ipc::decode_task(frame.data(), received.size, oxmbs::ipc::TaskProtocolLimits{64});
    assert(decoded && decoded.message.task_seq == first.task_seq);
    assert(registry.cancel_request(decoded.message.task_seq, decoded.message.session_id, decoded.message.deadline_unix_ms) ==
           oxmbs::ipc::TaskCorrelationStatus::Cancelled);
    assert(registry.pending_count() == 0);
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);
}

void test_oversized_low_frequency_payload_is_rejected_without_pending_task() {
    std::vector<std::uint8_t> oversized_payload(9, static_cast<std::uint8_t>('x'));  // 56 字节头 + 9 字节正文超过 64 字节协议上限。
    oxmbs::ipc::IpcQueueManager manager;
    oxmbs::ipc::TaskCorrelationRegistry registry;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);
    oxmbs::routing::WorkFuncRouter router(manager, registry, mock_route_policy(), router_options(64));

    const auto routed = router.route(make_message(601, kMockLoginMessageType, 1, oversized_payload));
    assert(routed.status == oxmbs::routing::WorkFuncRouteStatus::PayloadTooLarge);
    assert(registry.pending_count() == 0);
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);
}

}  // namespace

int main() {
    test_mock_login_is_copied_encoded_and_enqueued();
    test_mock_user_query_enters_worker_queue();
    test_high_frequency_messages_never_enter_ipc();
    test_invalid_low_frequency_message_is_rejected_before_registration();
    test_queue_full_is_non_blocking_and_rolls_back_second_task();
    test_oversized_low_frequency_payload_is_rejected_without_pending_task();
    return 0;
}
