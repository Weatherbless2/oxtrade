#include "oxapp/process/worker_supervisor.h"
#include "oxmbs/ipc/ipc_queue_manager.h"
#include "oxmbs/ipc/task_correlation.h"
#include "oxmbs/ipc/task_protocol.h"
#include "oxmbs/routing/message_route_policy.h"
#include "oxmbs/routing/mock_pack_message.h"
#include "oxmbs/routing/workfunc_router.h"
#include "oxmbs/worker/query_worker_runtime.h"
#include "oxmbs/worker/mock_query_service.h"
#include "oxmbs/worker/reply_collector.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

constexpr std::uint32_t kMockLoginMessageType = 9001;  // 延续 #6 的临时登录类型，仅用于 Mock 全链路验收。
constexpr std::uint32_t kMockUserQueryMessageType = 9002;  // 延续 #6 的临时用户查询类型；正式枚举到位后只替换策略注入。
constexpr long kRequestQueueMessageType = 17;  // System V Request mtype；与业务 msg_type 刻意分离。
constexpr long kReplyQueueMessageType = 18;  // System V Reply mtype；ReplyCollector 只消费此传输层类型。

int g_handler_call_count = 0;  // 只在同进程 consume_one 测试中观测，证明过期 Task 没有进入 Mock 业务函数。

struct FailOnceResponseSinkContext {
    bool failed_once{false};
    std::vector<oxmbs::ipc::IpcRespMsg> responses;
};

oxmbs::ipc::IpcQueueOptions queue_options() {
    oxmbs::ipc::IpcQueueOptions options;
    options.request.max_bytes = 4096;
    options.reply.max_bytes = 4096;
    options.request.permissions = 0600;
    options.reply.permissions = 0600;
    return options;
}

oxmbs::worker::QueryWorkerRuntimeOptions worker_options() {
    oxmbs::worker::QueryWorkerRuntimeOptions options;
    options.protocol_limits.max_frame_bytes = 4096;
    options.request_message_type = kRequestQueueMessageType;
    options.reply_message_type = kReplyQueueMessageType;
    return options;
}

oxmbs::worker::ReplyCollectorOptions collector_options() {
    oxmbs::worker::ReplyCollectorOptions options;
    options.protocol_limits.max_frame_bytes = 4096;
    options.reply_message_type = kReplyQueueMessageType;
    return options;
}

oxmbs::routing::WorkFuncRouterOptions router_options() {
    oxmbs::routing::WorkFuncRouterOptions options;
    options.protocol_limits.max_frame_bytes = 4096;
    options.query_task_timeout = std::chrono::milliseconds{1000};  // Mock 回环应在一秒内完成，真实部署后由配置注入。
    options.request_message_type = kRequestQueueMessageType;
    return options;
}

oxmbs::routing::MessageRoutePolicy route_policy() {
    return oxmbs::routing::MessageRoutePolicy(
        std::unordered_set<std::uint32_t>{kMockLoginMessageType, kMockUserQueryMessageType});
}

std::uint64_t now_unix_ms() {
    const auto now = std::chrono::system_clock::now();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
}

oxmbs::ipc::IpcTaskMsg make_task(oxmbs::ipc::TaskSequence task_seq,
                                  oxmbs::ipc::SessionId session_id,
                                  std::uint64_t deadline_unix_ms,
                                  std::uint32_t msg_type = kMockLoginMessageType) {
    oxmbs::ipc::IpcTaskMsg task;
    task.task_seq = task_seq;
    task.session_id = session_id;
    task.deadline_unix_ms = deadline_unix_ms;
    task.msg_type = msg_type;
    task.func_id = 42;
    task.payload = oxmbs::ipc::IpcPayload{'p', 'a', 'y', 'l', 'o', 'a', 'd'};
    return task;
}

oxmbs::ipc::IpcRespMsg make_response(const oxmbs::ipc::IpcTaskMsg& task) {
    oxmbs::ipc::IpcRespMsg response;
    response.task_seq = task.task_seq;
    response.session_id = task.session_id;
    response.deadline_unix_ms = task.deadline_unix_ms;
    response.msg_type = task.msg_type;
    response.func_id = task.func_id;
    response.status = oxmbs::ipc::IpcResponseStatus::Ok;
    response.payload = task.payload;
    return response;
}

void append_response(const oxmbs::ipc::IpcRespMsg& response, void* context) {
    auto* responses = static_cast<std::vector<oxmbs::ipc::IpcRespMsg>*>(context);
    responses->push_back(response);  // Mock 主进程 sink 仅在 ReplyCollector 的三重关联成功后被调用。
}

void append_timeout(const oxmbs::ipc::ExpiredTask& task, void* context) {
    auto* expired = static_cast<std::vector<oxmbs::ipc::ExpiredTask>*>(context);
    expired->push_back(task);  // Mock 超时 sink 只保存 task/session，未来由 HP-Socket 层决定如何组织超时回包。
}

void fail_once_response_sink(const oxmbs::ipc::IpcRespMsg& response, void* context) {
    auto* sink_context = static_cast<FailOnceResponseSinkContext*>(context);
    if (!sink_context->failed_once) {
        sink_context->failed_once = true;
        throw std::runtime_error("mock response sink failure");  // 第一次模拟 HP-Socket 会话或编码异常，Collector 不能因此退出。
    }
    sink_context->responses.push_back(response);  // 同一 Collector 下一条 Reply 必须仍能正常进入 sink。
}

oxmbs::worker::MockQueryExecutionResult counting_handler(const oxmbs::ipc::IpcTaskMsg& task, void*) {
    ++g_handler_call_count;
    oxmbs::worker::MockQueryExecutionResult result;
    result.payload = task.payload;
    return result;
}

oxmbs::worker::MockQueryExecutionResult throwing_handler(const oxmbs::ipc::IpcTaskMsg&, void*) {
    throw std::runtime_error("mock handler failure");  // 故意制造业务异常，验证 Worker 生成 Failed Reply 而不是进程崩溃。
}

void send_task(oxmbs::ipc::SystemVMessageQueue& request_queue, const oxmbs::ipc::IpcTaskMsg& task) {
    oxmbs::ipc::IpcPayload frame;
    std::string error;
    assert(oxmbs::ipc::encode_task(task, frame, error) == oxmbs::ipc::TaskProtocolStatus::Ok);
    assert(request_queue.send(kRequestQueueMessageType, frame.data(), frame.size(), false, error) ==
           oxmbs::ipc::IpcResult::Ok);
}

void send_response(oxmbs::ipc::SystemVMessageQueue& reply_queue, const oxmbs::ipc::IpcRespMsg& response) {
    oxmbs::ipc::IpcPayload frame;
    std::string error;
    assert(oxmbs::ipc::encode_response(response, frame, error) == oxmbs::ipc::TaskProtocolStatus::Ok);
    assert(reply_queue.send(kReplyQueueMessageType, frame.data(), frame.size(), false, error) ==
           oxmbs::ipc::IpcResult::Ok);
}

void test_supervised_mock_login_round_trip() {
    // 真正的父子进程链路：#6 路由写 Request，#7 子进程阻塞接收并 Mock 回显，主进程 Collector 精确关联后才调用 sink。
    oxmbs::ipc::IpcQueueManager manager;
    oxmbs::ipc::TaskCorrelationRegistry registry;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);

    oxmbs::worker::MockQueryService mock_service({kMockLoginMessageType, kMockUserQueryMessageType});
    oxmbs::worker::QueryWorkerRuntime runtime(
        manager.request_queue(), manager.reply_queue(), worker_options(), &oxmbs::worker::MockQueryService::handle, &mock_service);
    oxapp::process::WorkerSupervisor supervisor(1, &oxmbs::worker::query_worker_runtime_entry, &runtime);
    assert(supervisor.start());  // #2 Supervisor 启动真实 Worker，Worker 只借用父进程的队列对象。

    std::vector<oxmbs::ipc::IpcRespMsg> responses;
    oxmbs::worker::ReplyCollector collector(
        manager.reply_queue(), registry, collector_options(), &append_response, &responses);
    oxmbs::routing::WorkFuncRouter router(manager, registry, route_policy(), router_options());
    std::vector<std::uint8_t> login_payload{'{', '"', 'u', 's', 'e', 'r', '"', ':', '"', 'a', 'l', 'i', 'c', 'e', '"', '}'};
    const oxmbs::routing::MockPackMessage login{7001, kMockLoginMessageType, 42, login_payload.data(), login_payload.size()};
    const auto forwarded = router.route(login);
    assert(forwarded.forwarded());

    const auto collected = collector.collect_one();  // 阻塞等 Worker 的 Reply；高频 WorkFunc 不参与此等待。
    assert(collected.status == oxmbs::worker::ReplyCollectorStepStatus::DeliveredToSink);
    assert(collected.task_seq == forwarded.task_seq);
    assert(responses.size() == 1);
    assert(responses.front().task_seq == forwarded.task_seq);
    assert(responses.front().session_id == login.session_id);
    assert(responses.front().msg_type == kMockLoginMessageType);
    assert(responses.front().func_id == login.func_id);
    assert(std::string(responses.front().payload.begin(), responses.front().payload.end()) ==
           "{\"ok\":true,\"user\":{\"id\":\"mock-user-1001\",\"name\":\"Mock Alice\",\"role\":\"operator\"}}");  // Mock 登录返回固定用户数据，不读取真实密码或数据库。
    assert(registry.pending_count() == 0);

    const std::vector<std::uint8_t> user_query_payload{'m', 'o', 'c', 'k', '-', 'u', 's', 'e', 'r', '-', '1', '0', '0', '1'};
    const oxmbs::routing::MockPackMessage user_query{
        7001, kMockUserQueryMessageType, 43, user_query_payload.data(), user_query_payload.size()};
    const auto query_forwarded = router.route(user_query);
    assert(query_forwarded.forwarded());
    const auto query_collected = collector.collect_one();
    assert(query_collected.status == oxmbs::worker::ReplyCollectorStepStatus::DeliveredToSink);
    assert(responses.size() == 2);
    assert(responses.back().func_id == user_query.func_id);
    assert(std::string(responses.back().payload.begin(), responses.back().payload.end()) ==
           "{\"id\":\"mock-user-1001\",\"name\":\"Mock Alice\",\"status\":\"active\",\"roles\":[\"operator\"]}");  // 模拟查询与登录共用同一 Query Worker/Reply 回收链路。
    assert(registry.pending_count() == 0);

    assert(supervisor.stop(std::chrono::milliseconds{500}));  // Supervisor 回收阻塞 Worker；Worker 没有 IPC 删除责任。
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);
}

void test_expired_task_is_dropped_before_handler_and_reply() {
    oxmbs::ipc::IpcQueueManager manager;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);
    g_handler_call_count = 0;
    oxmbs::worker::QueryWorkerRuntime runtime(
        manager.request_queue(), manager.reply_queue(), worker_options(), &counting_handler);
    send_task(manager.request_queue(), make_task(1, 8001, now_unix_ms() - 1));

    const auto result = runtime.consume_one();
    assert(result.status == oxmbs::worker::QueryWorkerStepStatus::TaskExpiredDropped);
    assert(g_handler_call_count == 0);  // deadline 在业务入口之前检查，过期任务绝不能执行 Mock 或未来 LBM。
    std::vector<std::uint8_t> reply_buffer(4096);
    const auto reply = manager.reply_queue().receive(kReplyQueueMessageType, reply_buffer.data(), reply_buffer.size(), true);
    assert(!reply && reply.result == oxmbs::ipc::IpcResult::WouldBlock);  // 不产生成功或伪装的失败 Reply。
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);
}

void test_two_supervised_workers_compete_for_multiple_tasks() {
    // 同质 Worker 没有固定编号分配任务：两个子进程都阻塞同一 Request Queue，System V 内核自然唤醒可消费者。
    oxmbs::ipc::IpcQueueManager manager;
    oxmbs::ipc::TaskCorrelationRegistry registry;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);
    oxmbs::worker::MockQueryService mock_service({kMockLoginMessageType, kMockUserQueryMessageType});
    oxmbs::worker::QueryWorkerRuntime runtime(
        manager.request_queue(), manager.reply_queue(), worker_options(), &oxmbs::worker::MockQueryService::handle, &mock_service);
    oxapp::process::WorkerSupervisor supervisor(2, &oxmbs::worker::query_worker_runtime_entry, &runtime);
    assert(supervisor.start());
    assert(supervisor.running_count() == 2);

    std::vector<oxmbs::ipc::IpcRespMsg> responses;
    oxmbs::worker::ReplyCollector collector(
        manager.reply_queue(), registry, collector_options(), &append_response, &responses);
    oxmbs::routing::WorkFuncRouter router(manager, registry, route_policy(), router_options());
    const std::vector<std::uint8_t> payload{'q'};
    for (std::uint64_t session_id = 7101; session_id < 7105; ++session_id) {
        const oxmbs::routing::MockPackMessage message{
            session_id, kMockUserQueryMessageType, 42, payload.data(), payload.size()};
        assert(router.route(message).forwarded());
    }

    for (int index = 0; index < 4; ++index) {
        assert(collector.collect_one().status == oxmbs::worker::ReplyCollectorStepStatus::DeliveredToSink);
    }
    assert(responses.size() == 4);
    assert(registry.pending_count() == 0);  // 每条由任一 Worker 处理的 Response 都必须回到原 pending_ 记录。
    assert(supervisor.stop(std::chrono::milliseconds{500}));
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);
}

void test_invalid_task_is_dropped_and_worker_continues() {
    oxmbs::ipc::IpcQueueManager manager;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);
    oxmbs::worker::QueryWorkerRuntime runtime(manager.request_queue(), manager.reply_queue(), worker_options());
    const std::uint8_t invalid_frame[] = {0x00U, 0x01U, 0x02U};
    assert(manager.request_queue().send(kRequestQueueMessageType, invalid_frame, sizeof(invalid_frame), false, error) ==
           oxmbs::ipc::IpcResult::Ok);

    assert(runtime.consume_one().status == oxmbs::worker::QueryWorkerStepStatus::InvalidTaskDropped);
    send_task(manager.request_queue(), make_task(2, 8002, now_unix_ms() + 1000));
    assert(runtime.consume_one().status == oxmbs::worker::QueryWorkerStepStatus::ResponseSent);  // 坏帧被消费后，后续合法 Task 仍可正常处理。
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);
}

void test_handler_exception_returns_failed_response() {
    oxmbs::ipc::IpcQueueManager manager;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);
    oxmbs::worker::QueryWorkerRuntime runtime(
        manager.request_queue(), manager.reply_queue(), worker_options(), &throwing_handler);
    send_task(manager.request_queue(), make_task(3, 8003, now_unix_ms() + 1000));

    const auto processed = runtime.consume_one();
    assert(processed.status == oxmbs::worker::QueryWorkerStepStatus::ResponseSent);
    assert(processed.response_status == oxmbs::ipc::IpcResponseStatus::Failed);
    std::vector<std::uint8_t> reply_buffer(4096);
    const auto received = manager.reply_queue().receive(kReplyQueueMessageType, reply_buffer.data(), reply_buffer.size(), true);
    assert(received);
    const auto decoded = oxmbs::ipc::decode_response(reply_buffer.data(), received.size);
    assert(decoded && decoded.message.status == oxmbs::ipc::IpcResponseStatus::Failed);
    assert(std::string(decoded.message.payload.begin(), decoded.message.payload.end()).find("mock handler failure") != std::string::npos);
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);
}

void test_mock_service_rejects_unmapped_message_type() {
    oxmbs::ipc::IpcQueueManager manager;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);
    oxmbs::worker::MockQueryService mock_service({kMockLoginMessageType, kMockUserQueryMessageType});
    oxmbs::worker::QueryWorkerRuntime runtime(
        manager.request_queue(), manager.reply_queue(), worker_options(), &oxmbs::worker::MockQueryService::handle, &mock_service);
    send_task(manager.request_queue(), make_task(4, 8004, now_unix_ms() + 1000, 9999));  // 直接注入未映射类型，验证 Worker 不会把未知请求伪装成成功登录/查询。

    const auto processed = runtime.consume_one();
    assert(processed.status == oxmbs::worker::QueryWorkerStepStatus::ResponseSent);
    assert(processed.response_status == oxmbs::ipc::IpcResponseStatus::Rejected);
    std::vector<std::uint8_t> reply_buffer(4096);
    const auto received = manager.reply_queue().receive(kReplyQueueMessageType, reply_buffer.data(), reply_buffer.size(), true);
    assert(received);
    const auto decoded = oxmbs::ipc::decode_response(reply_buffer.data(), received.size);
    assert(decoded && decoded.message.status == oxmbs::ipc::IpcResponseStatus::Rejected);
    assert(std::string(decoded.message.payload.begin(), decoded.message.payload.end()).find("mock-unsupported-message-type") !=
           std::string::npos);
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);
}

void test_reply_collector_rejects_tampering_then_delivers_match() {
    oxmbs::ipc::IpcQueueManager manager;
    oxmbs::ipc::TaskCorrelationRegistry registry;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);
    const auto registration = registry.register_request(9001, std::chrono::seconds{1});
    assert(registration);
    const auto task = make_task(registration.task_seq, registration.session_id, registration.deadline_unix_ms);
    std::vector<oxmbs::ipc::IpcRespMsg> responses;
    oxmbs::worker::ReplyCollector collector(
        manager.reply_queue(), registry, collector_options(), &append_response, &responses);

    auto tampered = make_response(task);
    ++tampered.deadline_unix_ms;  // 模拟 Worker 或链路篡改 deadline，Collector 必须拒绝且不得误回包。
    send_response(manager.reply_queue(), tampered);
    const auto rejected = collector.collect_one();
    assert(rejected.status == oxmbs::worker::ReplyCollectorStepStatus::CorrelationRejected);
    assert(rejected.correlation_status == oxmbs::ipc::TaskCorrelationStatus::DeadlineMismatch);
    assert(responses.empty());
    assert(registry.pending_count() == 1);

    send_response(manager.reply_queue(), make_response(task));
    const auto delivered = collector.collect_one();
    assert(delivered.status == oxmbs::worker::ReplyCollectorStepStatus::DeliveredToSink);
    assert(responses.size() == 1);
    assert(registry.pending_count() == 0);
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);
}

void test_reply_collector_survives_response_sink_exception() {
    oxmbs::ipc::IpcQueueManager manager;
    oxmbs::ipc::TaskCorrelationRegistry registry;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);
    const auto failed_registration = registry.register_request(9201, std::chrono::seconds{1});
    assert(failed_registration);
    const auto failed_task = make_task(
        failed_registration.task_seq, failed_registration.session_id, failed_registration.deadline_unix_ms);
    FailOnceResponseSinkContext sink_context;
    oxmbs::worker::ReplyCollector collector(
        manager.reply_queue(), registry, collector_options(), &fail_once_response_sink, &sink_context);
    send_response(manager.reply_queue(), make_response(failed_task));
    const auto sink_failed = collector.collect_one();
    assert(sink_failed.status == oxmbs::worker::ReplyCollectorStepStatus::ResponseSinkFailed);
    assert(registry.pending_count() == 0);  // 三重关联已消费，不能因回调失败重新发送同一 Reply。

    const auto next_registration = registry.register_request(9202, std::chrono::seconds{1});
    assert(next_registration);
    const auto next_task = make_task(
        next_registration.task_seq, next_registration.session_id, next_registration.deadline_unix_ms);
    send_response(manager.reply_queue(), make_response(next_task));
    assert(collector.collect_one().status == oxmbs::worker::ReplyCollectorStepStatus::DeliveredToSink);
    assert(sink_context.responses.size() == 1 && registry.pending_count() == 0);  // 同一 Collector 在前一回调异常后仍可处理后续关联。
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);
}

void test_reply_collector_reports_expired_pending_tasks() {
    oxmbs::ipc::IpcQueueManager manager;
    oxmbs::ipc::TaskCorrelationRegistry registry;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);
    const auto registration = registry.register_request(9101, std::chrono::milliseconds{1});
    assert(registration);
    std::vector<oxmbs::ipc::ExpiredTask> timeout_events;
    oxmbs::worker::ReplyCollector collector(
        manager.reply_queue(), registry, collector_options(), nullptr, nullptr, &append_timeout, &timeout_events);

    std::this_thread::sleep_for(std::chrono::milliseconds{5});  // 等待 #5 steady_clock 超时，再由主进程 Collector 统一取走快照。
    const auto expired = collector.expire_pending();
    assert(expired.size() == 1);
    assert(expired.front().task_seq == registration.task_seq);
    assert(timeout_events.size() == 1 && timeout_events.front().session_id == registration.session_id);
    assert(registry.pending_count() == 0);
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);
}

void test_queue_removal_stops_blocked_runtime_and_collector() {
#if defined(__linux__)
    // 分别 fork Worker 和 Collector，验证主进程 IPC_RMID 会唤醒其阻塞 msgrcv；两个子进程均只退出，绝不自己 remove 队列。
    oxmbs::ipc::IpcQueueManager manager;
    oxmbs::ipc::TaskCorrelationRegistry registry;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);
    oxmbs::worker::QueryWorkerRuntime runtime(manager.request_queue(), manager.reply_queue(), worker_options());
    const pid_t worker_pid = ::fork();
    assert(worker_pid >= 0);
    if (worker_pid == 0) {
        _exit(runtime.run());  // _exit 防止子进程析构时误触发 RAII 队列删除；所有权始终在父进程。
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    assert(manager.request_queue().remove(error) == oxmbs::ipc::IpcResult::Ok);
    int worker_status = 0;
    assert(::waitpid(worker_pid, &worker_status, 0) == worker_pid);
    assert(WIFEXITED(worker_status) && WEXITSTATUS(worker_status) == 0);

    oxmbs::worker::ReplyCollector collector(manager.reply_queue(), registry, collector_options());
    const pid_t collector_pid = ::fork();
    assert(collector_pid >= 0);
    if (collector_pid == 0) {
        _exit(collector.run());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    assert(manager.reply_queue().remove(error) == oxmbs::ipc::IpcResult::Ok);
    int collector_status = 0;
    assert(::waitpid(collector_pid, &collector_status, 0) == collector_pid);
    assert(WIFEXITED(collector_status) && WEXITSTATUS(collector_status) == 0);
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);
#endif
}

}  // namespace

int main() {
    test_supervised_mock_login_round_trip();
    test_expired_task_is_dropped_before_handler_and_reply();
    test_two_supervised_workers_compete_for_multiple_tasks();
    test_invalid_task_is_dropped_and_worker_continues();
    test_handler_exception_returns_failed_response();
    test_mock_service_rejects_unmapped_message_type();
    test_reply_collector_rejects_tampering_then_delivers_match();
    test_reply_collector_survives_response_sink_exception();
    test_reply_collector_reports_expired_pending_tasks();
    test_queue_removal_stops_blocked_runtime_and_collector();
    return 0;
}
