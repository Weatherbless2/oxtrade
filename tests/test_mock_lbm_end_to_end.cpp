#include "oxapp/process/worker_supervisor.h"
#include "oxmbs/ipc/ipc_queue_manager.h"
#include "oxmbs/ipc/task_correlation.h"
#include "oxmbs/lbm/lbm_task_dispatcher.h"
#include "oxmbs/mock/mock_backend_protocol.h"
#include "oxmbs/routing/message_route_policy.h"
#include "oxmbs/routing/mock_pack_message.h"
#include "oxmbs/routing/workfunc_router.h"
#include "oxmbs/worker/query_worker_runtime.h"
#include "oxmbs/worker/reply_collector.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

constexpr long kRequestMessageType{501};
constexpr long kReplyMessageType{502};

std::filesystem::path temporary_directory() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() / ("oxmbs_mock_lbm_e2e_" + std::to_string(suffix));
    std::filesystem::create_directories(directory);  // 测试独占 XML 目录，避免与真实配置或并行测试混用。
    return directory;
}

std::filesystem::path write_funclist(const std::filesystem::path& directory) {
    const auto path = directory / "funclist.xml";
    std::ofstream stream(path, std::ios::binary);
    assert(stream);
    stream << "<funclist>\n";
    const std::uint32_t functions[] = {
        oxmbs::mock::kMockPageFunctionId, oxmbs::mock::kMockCreateFunctionId,
        oxmbs::mock::kMockUpdateFunctionId, oxmbs::mock::kMockTransitionFunctionId,
    };
    for (const auto function_id : functions) {
        stream << "  <function msg_type=\"" << oxmbs::mock::kMockRightMessageType << "\" func_id=\"" << function_id
               << "\" library=\"" << OXMBS_MOCK_LBM_LIBRARY << "\" symbol=\"oxmbs_lbm_mock_execute\" />\n";
    }
    stream << "</funclist>\n";
    assert(stream.good());
    return path;
}

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
    options.request_message_type = kRequestMessageType;
    options.reply_message_type = kReplyMessageType;
    return options;
}

oxmbs::worker::ReplyCollectorOptions collector_options() {
    oxmbs::worker::ReplyCollectorOptions options;
    options.reply_message_type = kReplyMessageType;
    return options;
}

oxmbs::routing::WorkFuncRouterOptions router_options() {
    oxmbs::routing::WorkFuncRouterOptions options;
    options.request_message_type = kRequestMessageType;
    options.query_task_timeout = std::chrono::seconds{2};
    return options;
}

void append_response(const oxmbs::ipc::IpcRespMsg& response, void* context) {
    auto* responses = static_cast<std::vector<oxmbs::ipc::IpcRespMsg>*>(context);
    responses->push_back(response);  // 只有 ReplyCollector 完成 task/session/deadline 三重匹配后，测试才能看到 LBM 响应。
}

std::string response_text(const oxmbs::ipc::IpcRespMsg& response) {
    return {response.payload.begin(), response.payload.end()};
}

void route_and_collect(oxmbs::routing::WorkFuncRouter& router,
                       oxmbs::worker::ReplyCollector& collector,
                       const std::string& payload,
                       std::uint32_t function_id,
                       std::vector<oxmbs::ipc::IpcRespMsg>& responses) {
    const std::vector<std::uint8_t> bytes(payload.begin(), payload.end());
    const oxmbs::routing::MockPackMessage message{
        7001, oxmbs::mock::kMockRightMessageType, function_id, bytes.data(), bytes.size(),
    };
    assert(router.route(message).forwarded());  // 请求必须进入 #6 低频白名单和 #4 Request Queue，而不是直接调用 LBM。
    assert(collector.collect_one().status == oxmbs::worker::ReplyCollectorStepStatus::DeliveredToSink);
    assert(!responses.empty());
}

void test_mock_lbm_crud_round_trip_uses_completed_backend_pipeline() {
    const auto directory = temporary_directory();
    oxmbs::lbm::LbmTaskDispatcher dispatcher;
    assert(dispatcher.load_funclist(write_funclist(directory)));  // 启动期只校验 XML，.so 由 fork 后的 Worker 首次任务懒加载。

    oxmbs::ipc::IpcQueueManager queues;
    std::string error;
    assert(queues.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);
    oxmbs::worker::QueryWorkerRuntime runtime(
        queues.request_queue(), queues.reply_queue(), worker_options(), &oxmbs::lbm::LbmTaskDispatcher::handle, &dispatcher);
    oxapp::process::WorkerSupervisor supervisor(1, &oxmbs::worker::query_worker_runtime_entry, &runtime);
    assert(supervisor.start());  // 单个真实子进程保留 LBM 内存中的 Mock CRUD 状态，避免测试误用父进程直接函数调用。

    oxmbs::ipc::TaskCorrelationRegistry correlations;
    std::vector<oxmbs::ipc::IpcRespMsg> responses;
    oxmbs::worker::ReplyCollector collector(
        queues.reply_queue(), correlations, collector_options(), &append_response, &responses);
    oxmbs::routing::WorkFuncRouter router(
        queues, correlations,
        oxmbs::routing::MessageRoutePolicy(std::unordered_set<std::uint32_t>{oxmbs::mock::kMockRightMessageType}),
        router_options());

    route_and_collect(router, collector,
                      "page=1&page_size=2&__mock_request_id=page-1&__mock_session_id=mock-session&__mock_service=users&__mock_operation=page",
                      oxmbs::mock::kMockPageFunctionId, responses);
    assert(response_text(responses.back()).find("request_id=page-1") != std::string::npos);
    assert(response_text(responses.back()).find("%22total%22%3A40") != std::string::npos);  // Mock 数据量足以验证真实分页语义，不是单行演示数据。

    route_and_collect(router, collector,
                      "record.user_name=e2e-user&record.display_name=E2E%20User&__mock_request_id=create-1&__mock_session_id=mock-session&__mock_service=users&__mock_operation=create",
                      oxmbs::mock::kMockCreateFunctionId, responses);
    assert(response_text(responses.back()).find("user-041") != std::string::npos);  // 主键由 LBM 生成，客户端不携带 user_id。

    route_and_collect(router, collector,
                      "record_id=user-041&changes.display_name=E2E%20Updated&__mock_request_id=update-1&__mock_session_id=mock-session&__mock_service=users&__mock_operation=update",
                      oxmbs::mock::kMockUpdateFunctionId, responses);
    assert(response_text(responses.back()).find("E2E%20Updated") != std::string::npos);

    route_and_collect(router, collector,
                      "record_id=user-041&state=frozen&__mock_request_id=transition-1&__mock_session_id=mock-session&__mock_service=users&__mock_operation=transition",
                      oxmbs::mock::kMockTransitionFunctionId, responses);
    assert(response_text(responses.back()).find("frozen") != std::string::npos);
    assert(correlations.pending_count() == 0);  // 四次请求都由 ReplyCollector 精确消费，不能留下假超时任务。

    assert(supervisor.stop(std::chrono::milliseconds{500}));
    assert(queues.close(error) == oxmbs::ipc::IpcResult::Ok);  // 仍由父进程删除双队列，Worker/LBM 从未拥有 IPC_RMID 权限。
    std::filesystem::remove_all(directory);
}

}  // namespace

int main() {
    test_mock_lbm_crud_round_trip_uses_completed_backend_pipeline();
    return 0;
}
