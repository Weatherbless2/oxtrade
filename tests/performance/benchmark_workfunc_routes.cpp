#include "oxapp/process/worker_supervisor.h"
#include "oxmbs/ipc/ipc_queue_manager.h"
#include "oxmbs/ipc/task_correlation.h"
#include "oxmbs/lbm/lbm_task_dispatcher.h"
#include "oxmbs/routing/message_route_policy.h"
#include "oxmbs/routing/mock_pack_message.h"
#include "oxmbs/routing/workfunc_router.h"
#include "oxmbs/worker/query_worker_runtime.h"
#include "oxmbs/worker/reply_collector.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kLowFrequencyType{9101};  // 白名单中的登录/查询/管理类测试消息，必须进入 Query Worker。
constexpr std::uint32_t kHighFrequencyType{9102};  // 白名单外的交易/行情类测试消息，必须保留在 WorkFunc 直接处理。
constexpr std::uint32_t kEchoFunction{1};  // 两条路线调用相同的“复制并原样应答”业务，避免功能差异污染比较。
constexpr long kRequestQueueType{401};  // System V mtype 只用于内核队列筛选，与业务 msg_type 严格分离。
constexpr long kReplyQueueType{402};

struct Options {
    std::size_t warmup{1000};
    std::size_t samples{10000};
    std::size_t payload_bytes{256};
};

struct Summary {
    double minimum_us{0};
    double mean_us{0};
    double p50_us{0};
    double p95_us{0};
    double p99_us{0};
    double maximum_us{0};
    double operations_per_second{0};
};

struct SinkState {
    std::vector<std::uint8_t> expected;  // 保存本轮固定输入，ReplyCollector 交付后核对业务结果没有因跨进程而改变。
    std::size_t delivered{0};
    bool payload_matches{true};
};

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--warmup" && index + 1 < argc) {
            options.warmup = std::stoull(argv[++index]);
        } else if (argument == "--samples" && index + 1 < argc) {
            options.samples = std::stoull(argv[++index]);
        } else if (argument == "--payload-bytes" && index + 1 < argc) {
            options.payload_bytes = std::stoull(argv[++index]);
        } else {
            throw std::invalid_argument("usage: oxmbs_route_latency_benchmark [--warmup N] [--samples N] [--payload-bytes N]");
        }
    }
    const std::size_t maximum_payload = oxmbs::ipc::TaskProtocolLimits{}.max_frame_bytes -
                                        oxmbs::ipc::IpcWireLayout::kHeaderBytes;
    if (options.warmup == 0 || options.samples == 0 || options.payload_bytes == 0 ||
        options.payload_bytes > maximum_payload) {
        throw std::invalid_argument("warmup/samples/payload must be positive and payload must fit one IPC frame");
    }
    return options;
}

Summary summarize(std::vector<double> values, double elapsed_seconds) {
    std::sort(values.begin(), values.end());
    const auto percentile = [&values](double ratio) {
        const auto rank = static_cast<std::size_t>(std::ceil(ratio * static_cast<double>(values.size())));
        return values[std::max<std::size_t>(1, rank) - 1];  // 最近秩百分位返回真实观测值，不通过插值制造不存在的延迟。
    };
    Summary result;
    result.minimum_us = values.front();
    result.mean_us = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    result.p50_us = percentile(0.50);
    result.p95_us = percentile(0.95);
    result.p99_us = percentile(0.99);
    result.maximum_us = values.back();
    result.operations_per_second = static_cast<double>(values.size()) / elapsed_seconds;
    return result;
}

void collect_response(const oxmbs::ipc::IpcRespMsg& response, void* context) {
    auto* state = static_cast<SinkState*>(context);
    ++state->delivered;  // 只有 ReplyCollector 完成 task/session/deadline 三重匹配后，响应才会到达这里。
    state->payload_matches = state->payload_matches && response.status == oxmbs::ipc::IpcResponseStatus::Ok &&
                             response.payload == state->expected;  // 同时校验状态、长度和每个字节，不能只用响应次数冒充业务闭环。
}

std::filesystem::path write_funclist() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("oxmbs_route_latency_" + std::to_string(Clock::now().time_since_epoch().count()) + ".xml");
    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot create benchmark funclist");
    }
    stream << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           << "<funclist>\n"
           << "  <function msg_type=\"" << kLowFrequencyType << "\" func_id=\"" << kEchoFunction
           << "\" library=\"" << OXMBS_ROUTE_LATENCY_LBM_LIBRARY
           << "\" symbol=\"oxmbs_route_latency_echo\" />\n"
           << "</funclist>\n";  // 启动期只登记路由；真正 dlopen/dlsym 仍发生在 Worker 首条预热任务中。
    if (!stream.good()) {
        throw std::runtime_error("cannot write benchmark funclist");
    }
    return path;
}

oxmbs::ipc::IpcQueueOptions queue_options() {
    oxmbs::ipc::IpcQueueOptions options;
    options.request.max_bytes = oxmbs::ipc::TaskProtocolLimits{}.max_frame_bytes;
    options.reply.max_bytes = oxmbs::ipc::TaskProtocolLimits{}.max_frame_bytes;
    options.request.permissions = 0600;  // 基准队列仅限当前用户，结束时由 manager 唯一执行 IPC_RMID。
    options.reply.permissions = 0600;
    return options;
}

oxmbs::worker::QueryWorkerRuntimeOptions worker_options() {
    oxmbs::worker::QueryWorkerRuntimeOptions options;
    options.request_message_type = kRequestQueueType;
    options.reply_message_type = kReplyQueueType;
    return options;
}

oxmbs::worker::ReplyCollectorOptions collector_options() {
    oxmbs::worker::ReplyCollectorOptions options;
    options.reply_message_type = kReplyQueueType;
    return options;
}

oxmbs::routing::WorkFuncRouterOptions router_options() {
    oxmbs::routing::WorkFuncRouterOptions options;
    options.query_task_timeout = std::chrono::seconds{5};  // 性能测试不给正常样本制造超时，deadline 只继续验证跨进程关联字段。
    options.request_message_type = kRequestQueueType;
    return options;
}

std::vector<std::uint8_t> direct_echo(const oxmbs::routing::MockPackMessage& message) {
    return {message.data, message.data + message.data_len};  // 高频核心做与 LBM 完全相同的一次正文复制，然后在当前进程立即得到应答。
}

void run_high_once(oxmbs::routing::WorkFuncRouter& router,
                   const oxmbs::routing::MockPackMessage& message,
                   const std::vector<std::uint8_t>& expected) {
    const auto route = router.route(message);  // WorkFunc 只做类型白名单判断；高频不复制、不登记、不访问 System V 队列。
    if (route.status != oxmbs::routing::WorkFuncRouteStatus::HighFrequencyPassthrough) {
        throw std::runtime_error("high-frequency message did not stay on direct route");
    }
    if (direct_echo(message) != expected) {
        throw std::runtime_error("high-frequency direct handler changed the payload");
    }
}

void run_low_once(oxmbs::routing::WorkFuncRouter& router,
                  oxmbs::worker::ReplyCollector& collector,
                  const oxmbs::routing::MockPackMessage& message) {
    const auto route = router.route(message);  // 低频路径在这里深拷贝、登记、编码并以 IPC_NOWAIT 写入 Request Queue。
    if (!route.forwarded()) {
        throw std::runtime_error("low-frequency route failed: " + route.error);
    }
    const auto collected = collector.collect_one();  // 当前基准按单次往返阻塞等待，终点覆盖 Worker/LBM/Reply Queue 和三重关联。
    if (collected.status != oxmbs::worker::ReplyCollectorStepStatus::DeliveredToSink) {
        throw std::runtime_error("low-frequency reply collection failed: " + collected.error);
    }
}

void print_summary(const char* name, const Summary& value, std::size_t samples) {
    std::cout << std::left << std::setw(36) << name
              << " n=" << std::right << std::setw(7) << samples
              << " min=" << std::setw(9) << std::fixed << std::setprecision(3) << value.minimum_us
              << " mean=" << std::setw(9) << value.mean_us
              << " P50=" << std::setw(9) << value.p50_us
              << " P95=" << std::setw(9) << value.p95_us
              << " P99=" << std::setw(9) << value.p99_us
              << " max=" << std::setw(9) << value.maximum_us
              << " us throughput=" << std::setw(12) << std::setprecision(1) << value.operations_per_second << " op/s\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path funclist_path;
    try {
        const Options options = parse_options(argc, argv);
        std::vector<std::uint8_t> payload(options.payload_bytes);
        for (std::size_t index = 0; index < payload.size(); ++index) {
            payload[index] = static_cast<std::uint8_t>((index * 31U + 7U) & 0xFFU);  // 固定非文本字节包含 0，证明路线按长度传输而不是依赖 C 字符串。
        }

        funclist_path = write_funclist();
        oxmbs::lbm::LbmTaskDispatcher dispatcher;
        const auto loaded = dispatcher.load_funclist(funclist_path);
        if (!loaded) {
            throw std::runtime_error("cannot load benchmark funclist: " + loaded.error);
        }

        oxmbs::ipc::IpcQueueManager queues;
        std::string queue_error;
        if (queues.open(queue_options(), queue_error) != oxmbs::ipc::IpcResult::Ok) {
            throw std::runtime_error("cannot open benchmark queues: " + queue_error);
        }
        oxmbs::worker::QueryWorkerRuntime runtime(
            queues.request_queue(), queues.reply_queue(), worker_options(),
            &oxmbs::lbm::LbmTaskDispatcher::handle, &dispatcher);  // Worker 使用真实 Dispatcher 和动态库 ABI，不用基准函数绕开 LBM。
        oxapp::process::WorkerSupervisor supervisor(
            1, &oxmbs::worker::query_worker_runtime_entry, &runtime);  // 单 Worker 与当前 oxMbsMockBackend 拓扑一致，避免并发度变化影响对照。
        if (!supervisor.start()) {
            throw std::runtime_error("cannot start benchmark Query Worker");
        }

        oxmbs::ipc::TaskCorrelationRegistry correlations;
        SinkState sink{payload};
        oxmbs::worker::ReplyCollector collector(
            queues.reply_queue(), correlations, collector_options(), &collect_response, &sink);
        oxmbs::routing::WorkFuncRouter router(
            queues, correlations,
            oxmbs::routing::MessageRoutePolicy(std::unordered_set<std::uint32_t>{kLowFrequencyType}),
            router_options());
        const oxmbs::routing::MockPackMessage high_message{
            7001, kHighFrequencyType, kEchoFunction, payload.data(), payload.size(),
        };
        const oxmbs::routing::MockPackMessage low_message{
            7001, kLowFrequencyType, kEchoFunction, payload.data(), payload.size(),
        };

        for (std::size_t index = 0; index < options.warmup; ++index) {
            run_high_once(router, high_message, payload);
            run_low_once(router, collector, low_message);  // 预热覆盖 fork 调度、容器容量和 LBM 首次 dlopen/dlsym，正式统计只看稳态。
        }

        std::vector<double> high_samples;
        high_samples.reserve(options.samples);
        const auto high_total_start = Clock::now();
        for (std::size_t index = 0; index < options.samples; ++index) {
            const auto start = Clock::now();
            run_high_once(router, high_message, payload);
            high_samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
        }
        const double high_total_seconds = std::chrono::duration<double>(Clock::now() - high_total_start).count();

        std::vector<double> low_samples;
        low_samples.reserve(options.samples);
        const auto low_total_start = Clock::now();
        for (std::size_t index = 0; index < options.samples; ++index) {
            const auto start = Clock::now();
            run_low_once(router, collector, low_message);
            low_samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - start).count());
        }
        const double low_total_seconds = std::chrono::duration<double>(Clock::now() - low_total_start).count();

        const Summary high = summarize(std::move(high_samples), high_total_seconds);
        const Summary low = summarize(std::move(low_samples), low_total_seconds);
        if (!sink.payload_matches || sink.delivered != options.warmup + options.samples || correlations.pending_count() != 0) {
            throw std::runtime_error("benchmark response validation or correlation cleanup failed");
        }

        std::cout << "payload_bytes=" << options.payload_bytes << " warmup=" << options.warmup
                  << " samples=" << options.samples << "\n";
        print_summary("high: WorkFunc -> direct echo", high, options.samples);
        print_summary("low: SysV -> Worker -> LBM -> reply", low, options.samples);
        std::cout << "mean_latency_ratio=" << std::fixed << std::setprecision(2) << low.mean_us / high.mean_us
                  << " low_extra_latency_us=" << std::setprecision(3) << low.mean_us - high.mean_us
                  << " direct_latency_reduction_percent=" << std::setprecision(2)
                  << (1.0 - high.mean_us / low.mean_us) * 100.0 << "\n";

        if (!supervisor.stop(std::chrono::milliseconds{1000})) {
            throw std::runtime_error("benchmark Worker did not stop cleanly");
        }
        if (queues.close(queue_error) != oxmbs::ipc::IpcResult::Ok) {
            throw std::runtime_error("benchmark queues did not close cleanly: " + queue_error);
        }
        std::filesystem::remove(funclist_path);  // 只删除本进程创建的临时 XML；动态库构建产物由 CMake 管理。
        return 0;
    } catch (const std::exception& error) {
        if (!funclist_path.empty()) {
            std::error_code ignored;
            std::filesystem::remove(funclist_path, ignored);  // 启动中途失败也回收临时路由文件，队列和 Worker 由各自 RAII 析构兜底。
        }
        std::cerr << "route latency benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
