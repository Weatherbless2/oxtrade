#include "oxmbs/lbm/lbm_abi.h"
#include "oxmbs/lbm/qmt_lbm_host.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

namespace {

using oxmbs::ipc::IpcTaskMsg;
using oxmbs::lbm::LbmRequestView;
using oxmbs::lbm::LbmResponseBuffer;
using oxmbs::lbm::LbmResponseCode;
using oxmbs::lbm::QmtLbmHostContext;

int reject_invalid_call(LbmResponseBuffer* response) {
    if (response == nullptr || response->payload == nullptr) {
        return 31;
    }
    constexpr char kMessage[] = "invalid QMT LBM host or request";
    if (response->payload_capacity < sizeof(kMessage) - 1) {
        return 32;
    }
    response->abi_version = oxmbs::lbm::kLbmAbiVersion;
    response->status = LbmResponseCode::Failed;
    std::memcpy(response->payload, kMessage, sizeof(kMessage) - 1);
    response->payload_size = sizeof(kMessage) - 1;
    return 0;
}

int execute_qmt_operation(const LbmRequestView* request,
                          LbmResponseBuffer* response,
                          std::uint32_t expected_function_id,
                          oxmbs::service::QmtOperation operation) {
    if (request == nullptr || response == nullptr || request->abi_version != oxmbs::lbm::kLbmAbiVersion ||
        request->func_id != expected_function_id || request->host_context == nullptr ||
        (request->payload == nullptr && request->payload_size != 0)) {
        return reject_invalid_call(response);  // ABI、路由键和借用输入均由宿主验证，模块绝不猜测或解引用未知地址。
    }

    const auto* host = static_cast<const QmtLbmHostContext*>(request->host_context);
    if (host->abi_version != oxmbs::lbm::kQmtLbmHostAbiVersion || host->repository == nullptr ||
        host->max_page_size == 0 || host->max_instrument_count == 0 || response->payload == nullptr ||
        response->payload_capacity == 0) {
        return reject_invalid_call(response);
    }

    oxmbs::service::QmtServiceRouterOptions options;
    options.routes = {{request->msg_type, expected_function_id, operation}};
    options.max_page_size = host->max_page_size;
    options.max_instrument_count = host->max_instrument_count;
    options.max_response_payload_bytes = std::min(host->max_response_payload_bytes, response->payload_capacity);
    options.require_authenticated_session = host->require_authenticated_session;
    oxmbs::service::QmtServiceRouter router(*host->repository, std::move(options));

    IpcTaskMsg task;
    task.task_seq = request->task_seq;
    task.session_id = request->session_id;
    task.deadline_unix_ms = request->deadline_unix_ms;
    task.msg_type = request->msg_type;
    task.func_id = request->func_id;
    task.payload.assign(request->payload, request->payload + request->payload_size);  // 模块先深拷贝借用正文，再交给 QMT 业务解析。
    auto result = router.execute(task);
    if (result.payload.size() > response->payload_capacity) {
        return 33;  // 宿主端口和响应缓冲区边界不一致时失败，不能截断 JSON 后假装成功。
    }

    response->abi_version = oxmbs::lbm::kLbmAbiVersion;
    response->status = result.ipc_status == oxmbs::ipc::IpcResponseStatus::Ok
                           ? LbmResponseCode::Ok
                           : (result.ipc_status == oxmbs::ipc::IpcResponseStatus::Rejected
                                  ? LbmResponseCode::Rejected
                                  : LbmResponseCode::Failed);
    std::memcpy(response->payload, result.payload.data(), result.payload.size());
    response->payload_size = result.payload.size();
    return 0;
}

}  // namespace

extern "C" int oxmbs_lbm_qmt_instrument_search(const LbmRequestView* request,
                                                 LbmResponseBuffer* response) {
    return execute_qmt_operation(request, response, 1, oxmbs::service::QmtOperation::InstrumentSearch);
}

extern "C" int oxmbs_lbm_qmt_calendar_page(const LbmRequestView* request,
                                             LbmResponseBuffer* response) {
    return execute_qmt_operation(request, response, 2, oxmbs::service::QmtOperation::CalendarPage);
}

extern "C" int oxmbs_lbm_qmt_quote_latest(const LbmRequestView* request,
                                            LbmResponseBuffer* response) {
    return execute_qmt_operation(request, response, 3, oxmbs::service::QmtOperation::QuoteLatest);
}
