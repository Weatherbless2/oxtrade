#include "oxapp/process/worker_supervisor.h"
#include "oxmbs/ipc/ipc_queue_manager.h"
#include "oxmbs/ipc/task_correlation.h"
#include "oxmbs/lbm/lbm_task_dispatcher.h"
#include "oxmbs/mock/mock_backend_protocol.h"
#include "oxmbs/mock/mock_gateway.h"
#include "oxmbs/routing/message_route_policy.h"
#include "oxmbs/routing/mock_pack_message.h"
#include "oxmbs/routing/workfunc_router.h"
#include "oxmbs/worker/query_worker_runtime.h"
#include "oxmbs/worker/reply_collector.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using Form = std::map<std::string, std::string>;

constexpr long kMockRequestQueueType{301};  // System V mtype 只供内核筛选，严格独立于 Mock-only 业务消息号。
constexpr long kMockReplyQueueType{302};

struct TransportRequest {
    std::string request_id;
    std::string session_id;
    std::string service;
    std::string operation;
    std::string payload;
    std::uint64_t deadline_unix_ms{0};
};

std::string url_encode(std::string_view source) {
    std::ostringstream output;
    output << std::uppercase << std::hex;
    for (const unsigned char character : source) {
        if (std::isalnum(character) != 0 || character == '-' || character == '_' || character == '.' ||
            character == '~') {
            output << static_cast<char>(character);
        } else {
            output << '%' << std::setw(2) << std::setfill('0') << static_cast<unsigned int>(character);
        }
    }
    return output.str();
}

int hex_value(char character) noexcept {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

std::string url_decode(std::string_view source) {
    std::string output;
    output.reserve(source.size());
    for (std::size_t index = 0; index < source.size(); ++index) {
        if (source[index] == '%' && index + 2 < source.size()) {
            const int high = hex_value(source[index + 1]);
            const int low = hex_value(source[index + 2]);
            if (high >= 0 && low >= 0) {
                output.push_back(static_cast<char>((high << 4) | low));
                index += 2;
                continue;
            }
        }
        output.push_back(source[index] == '+' ? ' ' : source[index]);
    }
    return output;
}

Form parse_form(std::string_view source) {
    Form form;
    std::size_t start = 0;
    while (start <= source.size()) {
        const std::size_t end = source.find('&', start);
        const std::string_view pair = source.substr(start, end == std::string_view::npos ? source.size() - start : end - start);
        if (!pair.empty()) {
            const std::size_t separator = pair.find('=');
            const std::string key = url_decode(pair.substr(0, separator));
            if (!key.empty()) {
                form[key] = separator == std::string_view::npos ? std::string{} : url_decode(pair.substr(separator + 1));
            }
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return form;
}

std::string form_value(const Form& form, std::string_view key) {
    const auto found = form.find(std::string(key));
    return found == form.end() ? std::string{} : found->second;
}

std::vector<std::string_view> split_tabs(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (start <= line.size()) {
        const std::size_t end = line.find('\t', start);
        fields.push_back(line.substr(start, end == std::string_view::npos ? line.size() - start : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return fields;
}

bool parse_request_line(std::string_view line, TransportRequest& request, std::string& error) {
    const auto fields = split_tabs(line);
    if (fields.size() != 7 || fields[0] != "Q") {
        error = "expected Mock-only Q line with seven tab-separated fields";
        return false;
    }
    request.request_id = url_decode(fields[1]);
    request.session_id = url_decode(fields[2]);
    request.service = url_decode(fields[3]);
    request.operation = url_decode(fields[4]);
    request.payload = url_decode(fields[5]);
    try {
        request.deadline_unix_ms = std::stoull(std::string(fields[6]));
    } catch (...) {
        error = "request deadline is not an unsigned integer";
        return false;
    }
    if (request.request_id.empty() || request.session_id.empty() || request.service.empty() || request.operation.empty() ||
        request.deadline_unix_ms == 0) {
        error = "request_id, session_id, service, operation and deadline are required";
        return false;
    }
    return true;
}

std::uint64_t now_unix_ms() noexcept {
    const auto now = std::chrono::system_clock::now();
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    return value > 0 ? static_cast<std::uint64_t>(value) : 0U;
}

oxmbs::ipc::IpcQueueOptions queue_options() {
    oxmbs::ipc::IpcQueueOptions options;
    options.request.max_bytes = oxmbs::ipc::TaskProtocolLimits{}.max_frame_bytes;
    options.reply.max_bytes = oxmbs::ipc::TaskProtocolLimits{}.max_frame_bytes;
    options.request.permissions = 0600;
    options.reply.permissions = 0600;
    return options;
}

oxmbs::worker::QueryWorkerRuntimeOptions worker_options() {
    oxmbs::worker::QueryWorkerRuntimeOptions options;
    options.request_message_type = kMockRequestQueueType;
    options.reply_message_type = kMockReplyQueueType;
    return options;
}

oxmbs::worker::ReplyCollectorOptions collector_options() {
    oxmbs::worker::ReplyCollectorOptions options;
    options.reply_message_type = kMockReplyQueueType;
    return options;
}

std::string append_form_field(std::string payload, std::string_view key, std::string_view value) {
    if (!payload.empty()) {
        payload.push_back('&');
    }
    payload += url_encode(key);
    payload.push_back('=');
    payload += url_encode(value);
    return payload;
}

class MockBackend {
public:
    bool start(std::string& error) {
        if (!write_funclist(error)) {
            return false;
        }
        if (!dispatcher_.load_funclist(funclist_path_)) {
            error = "cannot load generated Mock-only funclist: " + funclist_path_.string();
            return false;
        }
        if (queues_.open(queue_options(), error) != oxmbs::ipc::IpcResult::Ok) {
            return false;
        }
        runtime_ = std::make_unique<oxmbs::worker::QueryWorkerRuntime>(
            queues_.request_queue(), queues_.reply_queue(), worker_options(), &oxmbs::lbm::LbmTaskDispatcher::handle, &dispatcher_);
        supervisor_ = std::make_unique<oxapp::process::WorkerSupervisor>(
            1, &oxmbs::worker::query_worker_runtime_entry, runtime_.get());  // 单 Worker 保持同一 Mock LBM 进程内 CRUD 状态，不能误演示成多进程共享内存。
        if (!supervisor_->start()) {
            error = "cannot start Mock-only Query Worker";
            std::string close_error;
            queues_.close(close_error);
            return false;
        }
        collector_ = std::make_unique<oxmbs::worker::ReplyCollector>(
            queues_.reply_queue(), correlations_, collector_options(), &MockBackend::on_response, this);
        collector_thread_ = std::thread([this] {
            collector_exit_code_ = collector_->run();  // ReplyCollector 独立阻塞读取 Reply Queue，stdin 主循环不承担 Worker 回包等待。
        });
        return true;
    }

    void serve(std::istream& input) {
        std::string line;
        while (std::getline(input, line)) {
            TransportRequest request;
            std::string error;
            if (!parse_request_line(line, request, error)) {
                emit_failure(request, "protocol_error", error);
                continue;
            }
            route_request(request);
        }
    }

    void stop() noexcept {
        if (supervisor_ != nullptr) {
            supervisor_->stop(std::chrono::milliseconds{500});  // 先回收 Worker，避免其继续使用即将删除的主进程队列。
        }
        std::string ignored;
        queues_.close(ignored);  // 父进程是 Queue 唯一删除者；IPC_RMID 会唤醒 ReplyCollector 线程。
        if (collector_thread_.joinable()) {
            collector_thread_.join();
        }
        collector_.reset();
        supervisor_.reset();
        runtime_.reset();
        if (!funclist_path_.empty()) {
            std::error_code ignored_error;
            std::filesystem::remove_all(funclist_path_.parent_path(), ignored_error);  // 只删除自己创建的临时 Mock 路由目录。
        }
    }

    ~MockBackend() { stop(); }

private:
    bool write_funclist(std::string& error) {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto directory = std::filesystem::temp_directory_path() / ("oxmbs_mock_backend_" + std::to_string(suffix));
        std::error_code filesystem_error;
        std::filesystem::create_directories(directory, filesystem_error);
        if (filesystem_error) {
            error = "cannot create Mock-only funclist directory: " + filesystem_error.message();
            return false;
        }
        funclist_path_ = directory / "funclist.xml";
        std::ofstream stream(funclist_path_, std::ios::binary);
        if (!stream) {
            error = "cannot create Mock-only funclist";
            return false;
        }
        stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<funclist>\n";
        const std::uint32_t message_types[] = {
            oxmbs::mock::kMockSystemMessageType, oxmbs::mock::kMockRightMessageType,
            oxmbs::mock::kMockFundMessageType, oxmbs::mock::kMockRiskMessageType,
        };
        const std::uint32_t function_ids[] = {
            oxmbs::mock::kMockPageFunctionId, oxmbs::mock::kMockHeartbeatFunctionId,
            oxmbs::mock::kMockLoginFunctionId, oxmbs::mock::kMockCreateFunctionId,
            oxmbs::mock::kMockUpdateFunctionId, oxmbs::mock::kMockTransitionFunctionId,
            oxmbs::mock::kMockLogoutFunctionId,
        };
        for (const auto message_type : message_types) {
            for (const auto function_id : function_ids) {
                stream << "  <function msg_type=\"" << message_type << "\" func_id=\"" << function_id
                       << "\" library=\"" << OXMBS_MOCK_LBM_LIBRARY
                       << "\" symbol=\"oxmbs_lbm_mock_execute\" />\n";  // XML 只暴露 Mock-only 白名单，#8 Loader 仍按首次任务懒加载 .so。
            }
        }
        stream << "</funclist>\n";
        if (!stream.good()) {
            error = "cannot write Mock-only funclist";
            return false;
        }
        return true;
    }

    void route_request(const TransportRequest& request) {
        if (request.service == "market" || request.service == "trade" || request.service == "query") {
            route_high_frequency_gateway(request);  // 行情/交易模拟高频核心直接走 Mock Gateway，不得错误进入低频 Query Worker。
            return;
        }
        const auto route = oxmbs::mock::resolve_mock_route(request.service, request.operation);
        if (!route) {
            emit_failure(request, "protocol_error", "unknown Mock-only service or operation");
            return;
        }
        const std::uint64_t current_time = now_unix_ms();
        if (current_time == 0 || request.deadline_unix_ms <= current_time) {
            emit_response_line(request.request_id, request.session_id, request.service, request.operation,
                               "timed_out", "timeout", "request deadline elapsed before Mock backend routing", "{}");
            return;
        }
        std::string payload = request.payload;
        payload = append_form_field(std::move(payload), "__mock_request_id", request.request_id);
        payload = append_form_field(std::move(payload), "__mock_session_id", request.session_id);
        payload = append_form_field(std::move(payload), "__mock_service", request.service);
        payload = append_form_field(std::move(payload), "__mock_operation", request.operation);
        std::vector<std::uint8_t> body(payload.begin(), payload.end());  // WorkFuncRouter 会再深拷贝；stdin 缓冲区不会跨 IPC 被 Worker 借用。
        const auto remaining = std::min<std::uint64_t>(request.deadline_unix_ms - current_time, 3000U);
        oxmbs::routing::WorkFuncRouterOptions options;
        options.request_message_type = kMockRequestQueueType;
        options.query_task_timeout = std::chrono::milliseconds{std::max<std::uint64_t>(1U, remaining)};  // #5 IPC deadline 从客户端绝对 deadline 剩余时间推导，不能无界延长。
        oxmbs::routing::MessageRoutePolicy policy({
            oxmbs::mock::kMockSystemMessageType, oxmbs::mock::kMockRightMessageType,
            oxmbs::mock::kMockFundMessageType, oxmbs::mock::kMockRiskMessageType,
        });
        oxmbs::routing::WorkFuncRouter router(queues_, correlations_, std::move(policy), options);
        const oxmbs::routing::MockPackMessage message{
            numeric_session_id(request.session_id), route.message_type, route.function_id, body.data(), body.size(),
        };
        const auto routed = router.route(message);
        if (!routed.forwarded()) {
            emit_failure(request, "service_error", "Mock backend route failed: " + routed.error);
        }
    }

    void route_high_frequency_gateway(const TransportRequest& request) {
        const std::uint64_t current_time = now_unix_ms();
        if (current_time == 0 || request.deadline_unix_ms <= current_time) {
            emit_response_line(request.request_id, request.session_id, request.service, request.operation,
                               "timed_out", "timeout", "request deadline elapsed before Mock gateway routing", "{}");
            return;
        }
        const Form form = parse_form(request.payload);
        const oxmbs::mock::MockGatewayResult result = gateway_.handle(request.service, request.operation, form);
        emit_response_line(
            request.request_id, request.session_id, request.service, request.operation,
            result.app_status, result.error_code, result.error_message, result.payload_json);  // 高低频均保持同一客户端关联/错误模型，页面无需知道路由差异。
    }

    std::uint64_t numeric_session_id(const std::string& session_id) {
        const auto found = session_ids_.find(session_id);
        if (found != session_ids_.end()) {
            return found->second;
        }
        const std::uint64_t assigned = next_session_id_++;
        session_ids_.emplace(session_id, assigned);  // IPC 只传稳定整数 session，文本 session 仍由 LBM envelope 原样回给 Python Tracker。
        return assigned;
    }

    static void on_response(const oxmbs::ipc::IpcRespMsg& response, void* context) {
        auto* backend = static_cast<MockBackend*>(context);
        const Form envelope = parse_form(std::string_view(
            reinterpret_cast<const char*>(response.payload.data()), response.payload.size()));
        const std::string request_id = form_value(envelope, "request_id");
        const std::string session_id = form_value(envelope, "session_id");
        if (request_id.empty() || session_id.empty()) {
            std::cerr << "MockBackend dropped uncorrelatable LBM response: "
                      << std::string(response.payload.begin(), response.payload.end()) << '\n';  // Mock-only 诊断输出只走 stderr，不能污染 Python 读取的 stdout 协议。
            return;  // 无法关联的 LBM 回包不写 stdout，Python Tracker 会按自身 deadline 收口，避免把错误数据交给任意页面。
        }
        backend->emit_response_line(
            request_id, session_id, form_value(envelope, "service"), form_value(envelope, "operation"),
            form_value(envelope, "app_status"), form_value(envelope, "error_code"), form_value(envelope, "error_message"),
            form_value(envelope, "payload_json"));
    }

    void emit_failure(const TransportRequest& request, const std::string& error_code, const std::string& message) {
        emit_response_line(request.request_id, request.session_id, request.service, request.operation,
                           "failed", error_code, message, "{}");
    }

    void emit_response_line(const std::string& request_id,
                            const std::string& session_id,
                            const std::string& service,
                            const std::string& operation,
                            const std::string& status,
                            const std::string& error_code,
                            const std::string& error_message,
                            const std::string& payload_json) {
        std::lock_guard<std::mutex> lock(stdout_mutex_);  // stdin 主线程的即时失败与 ReplyCollector 线程的异步成功共用 stdout，整行必须原子写出。
        std::cout << "R\t" << url_encode(request_id) << '\t' << url_encode(session_id) << '\t' << url_encode(service) << '\t'
                  << url_encode(operation) << '\t' << url_encode(status) << '\t' << url_encode(error_code) << '\t'
                  << url_encode(error_message) << '\t' << url_encode(payload_json) << '\n' << std::flush;
    }

    oxmbs::lbm::LbmTaskDispatcher dispatcher_;
    oxmbs::ipc::IpcQueueManager queues_;
    oxmbs::ipc::TaskCorrelationRegistry correlations_;
    std::unique_ptr<oxmbs::worker::QueryWorkerRuntime> runtime_;
    std::unique_ptr<oxapp::process::WorkerSupervisor> supervisor_;
    std::unique_ptr<oxmbs::worker::ReplyCollector> collector_;
    std::thread collector_thread_;
    int collector_exit_code_{-1};
    std::filesystem::path funclist_path_;
    std::map<std::string, std::uint64_t> session_ids_;
    std::uint64_t next_session_id_{1000};
    std::mutex stdout_mutex_;
    oxmbs::mock::MockGateway gateway_;  // Mock-only 行情/交易核心；正式 #14/#15 接入时替换为 oxMWH 与交易/查询核心进程。
};

}  // namespace

int main() {
    MockBackend backend;
    std::string error;
    if (!backend.start(error)) {
        std::cerr << "oxMbsMockBackend startup failed: " << error << '\n';
        return 1;
    }
    backend.serve(std::cin);  // stdin/stdout 是 #24 专用本地适配，正式 HP-Socket 接入时只替换这一层。
    backend.stop();
    return 0;
}
