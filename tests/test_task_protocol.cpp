#include "oxmbs/ipc/ipc_queue_manager.h"
#include "oxmbs/ipc/task_correlation.h"
#include "oxmbs/ipc/task_protocol.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using oxmbs::ipc::IpcPayload;
using oxmbs::ipc::IpcRespMsg;
using oxmbs::ipc::IpcResponseStatus;
using oxmbs::ipc::IpcTaskMsg;
using oxmbs::ipc::IpcWireLayout;
using oxmbs::ipc::SessionId;
using oxmbs::ipc::TaskCorrelationRegistry;
using oxmbs::ipc::TaskCorrelationStatus;
using oxmbs::ipc::TaskProtocolLimits;
using oxmbs::ipc::TaskProtocolStatus;

constexpr std::uint32_t kQueryMessageType = 1001;
constexpr std::uint32_t kQueryFunctionId = 42;

IpcTaskMsg make_task(std::uint64_t task_seq, SessionId session_id) {
    IpcTaskMsg task;
    task.task_seq = task_seq;
    task.session_id = session_id;
    task.msg_type = kQueryMessageType;
    task.func_id = kQueryFunctionId;
    task.payload = IpcPayload{static_cast<std::uint8_t>('q'), 0U, static_cast<std::uint8_t>('r')};  // 故意含 \0，证明协议按二进制长度处理而非 C 字符串。
    return task;
}

IpcRespMsg make_response(const IpcTaskMsg& task) {
    IpcRespMsg response;
    response.task_seq = task.task_seq;
    response.session_id = task.session_id;
    response.msg_type = task.msg_type;
    response.func_id = task.func_id;
    response.status = IpcResponseStatus::Ok;
    response.payload = IpcPayload{static_cast<std::uint8_t>('o'), static_cast<std::uint8_t>('k')};
    return response;
}

void write_u32_le(IpcPayload& bytes, std::size_t offset, std::uint32_t value) {
    for (std::uint32_t byte_index = 0; byte_index < 4; ++byte_index) {
        bytes[offset + byte_index] = static_cast<std::uint8_t>((value >> (byte_index * 8U)) & 0xFFU);
    }
}

oxmbs::ipc::IpcQueueOptions queue_options() {
    oxmbs::ipc::IpcQueueOptions options;
    options.request.max_bytes = 4096;
    options.reply.max_bytes = 4096;
    options.request.permissions = 0600;
    options.reply.permissions = 0600;
    return options;
}

void test_task_and_response_round_trip() {
    const auto task = make_task(88, 9001);
    IpcPayload task_frame;
    std::string error;
    assert(oxmbs::ipc::encode_task(task, task_frame, error) == TaskProtocolStatus::Ok);
    assert(task_frame.size() == IpcWireLayout::kHeaderBytes + task.payload.size());

    const auto decoded_task = oxmbs::ipc::decode_task(task_frame.data(), task_frame.size());
    assert(decoded_task);
    assert(decoded_task.message.task_seq == task.task_seq);
    assert(decoded_task.message.session_id == task.session_id);
    assert(decoded_task.message.msg_type == task.msg_type);
    assert(decoded_task.message.func_id == task.func_id);
    assert(decoded_task.message.payload == task.payload);

    auto response = make_response(task);
    response.status = IpcResponseStatus::Rejected;  // 非成功状态仍必须完整保留关联字段，让主进程可以把错误准确回给原会话。
    IpcPayload response_frame;
    assert(oxmbs::ipc::encode_response(response, response_frame, error) == TaskProtocolStatus::Ok);
    const auto decoded_response = oxmbs::ipc::decode_response(response_frame.data(), response_frame.size());
    assert(decoded_response);
    assert(decoded_response.message.task_seq == response.task_seq);
    assert(decoded_response.message.session_id == response.session_id);
    assert(decoded_response.message.status == IpcResponseStatus::Rejected);
    assert(decoded_response.message.payload == response.payload);
}

void test_wire_validation_rejects_corruption() {
    const auto task = make_task(89, 9002);
    IpcPayload frame;
    std::string error;
    assert(oxmbs::ipc::encode_task(task, frame, error) == TaskProtocolStatus::Ok);

    auto corrupted = frame;
    corrupted[IpcWireLayout::kMagicOffset] ^= 0xFFU;  // 制造非 oxMBS 报文，验证解码器在读取业务字段前先检查魔数。
    assert(oxmbs::ipc::decode_task(corrupted.data(), corrupted.size()).status == TaskProtocolStatus::InvalidMagic);

    corrupted = frame;
    corrupted[IpcWireLayout::kVersionOffset] = 2U;  // 版本不兼容时必须拒绝，不能按当前头布局继续猜测解析。
    assert(oxmbs::ipc::decode_task(corrupted.data(), corrupted.size()).status == TaskProtocolStatus::UnsupportedVersion);

    corrupted = frame;
    corrupted[IpcWireLayout::kKindOffset] = 2U;  // 把 Task 改成 Response 类型，验证请求解码入口不会接受错误方向的帧。
    assert(oxmbs::ipc::decode_task(corrupted.data(), corrupted.size()).status == TaskProtocolStatus::UnexpectedMessageKind);

    corrupted = frame;
    corrupted[IpcWireLayout::kHeaderBytesOffset] = 0U;
    assert(oxmbs::ipc::decode_task(corrupted.data(), corrupted.size()).status == TaskProtocolStatus::InvalidHeaderSize);

    corrupted = frame;
    write_u32_le(corrupted, IpcWireLayout::kFrameBytesOffset, 0U);  // 声明长度与实际 receive 字节数不符，阻止截断或拼接帧混入业务层。
    assert(oxmbs::ipc::decode_task(corrupted.data(), corrupted.size()).status == TaskProtocolStatus::InvalidFrameSize);

    corrupted = frame;
    write_u32_le(corrupted, IpcWireLayout::kPayloadBytesOffset, 0U);
    assert(oxmbs::ipc::decode_task(corrupted.data(), corrupted.size()).status == TaskProtocolStatus::InvalidPayloadSize);

    const TaskProtocolLimits too_small{IpcWireLayout::kHeaderBytes + 1};
    IpcPayload rejected_frame;
    assert(oxmbs::ipc::encode_task(task, rejected_frame, error, too_small) == TaskProtocolStatus::FrameTooLarge);
    assert(rejected_frame.empty());  // 编码长度检查失败时输出必须保持空，调用方不会误把部分内容送入队列。
    assert(oxmbs::ipc::decode_task(frame.data(), frame.size(), too_small).status == TaskProtocolStatus::FrameTooLarge);

    auto invalid_task = task;
    invalid_task.task_seq = 0;
    assert(oxmbs::ipc::encode_task(invalid_task, rejected_frame, error) == TaskProtocolStatus::InvalidField);
    assert(rejected_frame.empty());

    auto invalid_response = make_response(task);
    invalid_response.status = static_cast<IpcResponseStatus>(99U);
    assert(oxmbs::ipc::encode_response(invalid_response, rejected_frame, error) == TaskProtocolStatus::InvalidField);
}

void test_task_correlation_and_timeouts() {
    TaskCorrelationRegistry registry;
    const auto now = TaskCorrelationRegistry::Clock::time_point{} + std::chrono::seconds{10};

    const auto registration = registry.register_request(7001, std::chrono::milliseconds{100}, now);
    assert(registration);
    const auto task = make_task(registration.task_seq, registration.session_id);
    const auto matched = registry.resolve_response(make_response(task), now + std::chrono::milliseconds{20});
    assert(matched);
    assert(matched.session_id == registration.session_id);
    assert(registry.pending_count() == 0);

    const auto mismatch_registration = registry.register_request(7002, std::chrono::milliseconds{100}, now);
    auto wrong_session_response = make_response(make_task(mismatch_registration.task_seq, 7003));
    const auto mismatch = registry.resolve_response(wrong_session_response, now + std::chrono::milliseconds{20});
    assert(mismatch.status == TaskCorrelationStatus::SessionMismatch);
    assert(registry.pending_count() == 1);  // 错会话应答不能删除正确请求的等待记录。
    const auto recovered = registry.resolve_response(
        make_response(make_task(mismatch_registration.task_seq, mismatch_registration.session_id)), now + std::chrono::milliseconds{30});
    assert(recovered.status == TaskCorrelationStatus::Matched);

    const auto timeout_registration = registry.register_request(7004, std::chrono::milliseconds{10}, now);
    const auto timed_out = registry.resolve_response(
        make_response(make_task(timeout_registration.task_seq, timeout_registration.session_id)), now + std::chrono::milliseconds{10});
    assert(timed_out.status == TaskCorrelationStatus::TimedOut);

    const auto expired_registration = registry.register_request(7005, std::chrono::milliseconds{5}, now);
    const auto active_registration = registry.register_request(7006, std::chrono::seconds{1}, now);
    const auto expired = registry.expire(now + std::chrono::milliseconds{6});
    assert(expired.size() == 1);
    assert(expired.front().task_seq == expired_registration.task_seq);
    assert(registry.pending_count() == 1);
    assert(registry.resolve_response(make_response(make_task(active_registration.task_seq, active_registration.session_id)),
                                     now + std::chrono::milliseconds{7})
               .status == TaskCorrelationStatus::Matched);

    assert(registry.register_request(0, std::chrono::milliseconds{1}, now).status == TaskCorrelationStatus::InvalidArgument);
    assert(registry.register_request(7007, std::chrono::milliseconds::zero(), now).status == TaskCorrelationStatus::InvalidArgument);
    assert(registry.resolve_response(IpcRespMsg{}, now).status == TaskCorrelationStatus::InvalidArgument);
}

void test_parent_child_task_response() {
#if defined(__linux__)
    oxmbs::ipc::IpcQueueManager manager;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);

    TaskCorrelationRegistry registry;
    const auto now = TaskCorrelationRegistry::Clock::now();
    const auto registration = registry.register_request(8001, std::chrono::seconds{1}, now);
    assert(registration);
    const auto request = make_task(registration.task_seq, registration.session_id);
    IpcPayload request_frame;
    assert(oxmbs::ipc::encode_task(request, request_frame, error) == TaskProtocolStatus::Ok);

    const pid_t child = ::fork();
    assert(child >= 0);
    if (child == 0) {
        std::vector<std::uint8_t> request_buffer(TaskProtocolLimits{}.max_frame_bytes);
        const auto received = manager.request_queue().receive(1, request_buffer.data(), request_buffer.size(), false);
        if (!received) {
            _exit(21);  // Worker 模拟进程未能从 Request Queue 取到完整任务帧。
        }
        const auto decoded_request = oxmbs::ipc::decode_task(request_buffer.data(), received.size);
        if (!decoded_request) {
            _exit(22);  // 证明跨进程传递后的字节没有通过长度、版本或字段校验。
        }

        const auto response = make_response(decoded_request.message);  // 关键关联字段从请求解码结果原样带回，不在 Worker 端重新分配。
        IpcPayload response_frame;
        std::string child_error;
        if (oxmbs::ipc::encode_response(response, response_frame, child_error) != TaskProtocolStatus::Ok ||
            manager.reply_queue().send(2, response_frame.data(), response_frame.size(), false, child_error) !=
                oxmbs::ipc::IpcResult::Ok) {
            _exit(23);
        }
        _exit(0);
    }

    assert(manager.request_queue().send(1, request_frame.data(), request_frame.size(), false, error) ==
           oxmbs::ipc::IpcResult::Ok);
    std::vector<std::uint8_t> response_buffer(TaskProtocolLimits{}.max_frame_bytes);
    const auto received = manager.reply_queue().receive(2, response_buffer.data(), response_buffer.size(), false);
    assert(received);
    const auto decoded_response = oxmbs::ipc::decode_response(response_buffer.data(), received.size);
    assert(decoded_response);
    assert(registry.resolve_response(decoded_response.message, now + std::chrono::milliseconds{50}).status ==
           TaskCorrelationStatus::Matched);

    int child_status = 0;
    assert(::waitpid(child, &child_status, 0) == child);
    assert(WIFEXITED(child_status));
    assert(WEXITSTATUS(child_status) == 0);
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);
#endif
}

}  // namespace

int main() {
    test_task_and_response_round_trip();
    test_wire_validation_rejects_corruption();
    test_task_correlation_and_timeouts();
    test_parent_child_task_response();
    return 0;
}
