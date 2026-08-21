#include "oxmbs/config/mbs_config.h"
#include "oxmbs/ipc/ipc_queue_manager.h"
#include "oxmbs/logging/run_logger.h"
#include "oxmbs/lbm/lbm_task_dispatcher.h"
#include "oxmbs/runtime/system_info.h"
#include "oxmbs/worker/query_worker_runtime.h"
#include "oxmbs/worker/reply_collector.h"

#include "oxapp/process/worker_supervisor.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {

volatile std::sig_atomic_t stop_requested = 0;  // 主进程收到 SIGINT/SIGTERM 后只设置标志，实际关闭动作在普通线程上下文完成。

void handle_stop_signal(int) noexcept {
    stop_requested = 1;  // 信号处理器只做异步信号安全赋值，不能在这里写日志、删队列或 join 线程。
}

void install_main_signal_handlers() noexcept {
    struct sigaction action {};
    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

void print_usage(const char* program) {
    std::cout << "usage: " << program << " [config-path]\n";  // 命令行只允许一个可选配置路径。
}

oxmbs::ipc::IpcQueueOptions default_ipc_queue_options() {
    oxmbs::ipc::IpcQueueOptions options;
    const auto max_frame_bytes = oxmbs::ipc::TaskProtocolLimits{}.max_frame_bytes;
    options.request.max_bytes = max_frame_bytes;  // 当前临时总队列容量与单帧协议上限均为 4 KiB；后续由运行配置统一注入。
    options.reply.max_bytes = max_frame_bytes;
    options.request.permissions = 0600;
    options.reply.permissions = 0600;
    return options;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2) {  // 多余参数视为调用错误，不进入配置解析。
        print_usage(argv[0]);
        return 2;
    }

    const std::filesystem::path config_path = argc == 2 ? argv[1] : "config/mbsconfig.ini";  // 未传参使用默认路径。
    oxmbs::config::MbsConfig config;
    std::string error;
    if (!oxmbs::config::MbsConfigLoader::load(config_path, config, error)) {  // 配置失败时先输出错误，不创建日志器。
        std::cerr << "oxMBS configuration error: " << error << '\n';
        return 1;
    }

    oxmbs::logging::RunLogger logger;
    oxmbs::logging::RunLoggerOptions options;  // 将配置对象转换为日志器所需选项。
    options.directory = config.log_directory;
    options.service_name = config.service_name;
    options.minimum_level = config.log_level;
    if (!logger.open(options, error)) {  // 目录或文件不可写时终止启动。
        std::cerr << "oxMBS logger error: " << error << '\n';
        return 1;
    }
    install_main_signal_handlers();  // 在创建 IPC 和 fork Worker 前安装，确保启动过程收到 SIGTERM 也能进入统一清理路径；#7 Worker 子进程会恢复默认语义。

    oxmbs::runtime::SystemInfo system_info;
    if (!oxmbs::runtime::SystemInfoCollector::collect(config.log_directory, system_info, error)) {  // 系统信息失败记录到 run 日志。
        logger.error(error);
        return 1;
    }

    logger.info("service started; environment=" + config.environment);  // 启动摘要第一行。
    logger.info("listen=" + config.listen_host + ":" + std::to_string(config.listen_port));
    logger.info("query_worker_count=" + std::to_string(config.query_worker_count));
    logger.info("cpu=" + system_info.cpu_model + ", logical_cpu_count=" +
                std::to_string(system_info.logical_cpu_count));
    logger.info("memory_total_bytes=" + std::to_string(system_info.memory_total_bytes) +
                ", memory_available_bytes=" + std::to_string(system_info.memory_available_bytes));
    logger.info("disk_total_bytes=" + std::to_string(system_info.disk_total_bytes) +
                ", disk_available_bytes=" + std::to_string(system_info.disk_available_bytes));

    oxmbs::lbm::LbmTaskDispatcher lbm_dispatcher;  // 主进程创建 Loader；fork 后 Worker 继承已校验路由但按需加载库。
    const auto funclist_result = lbm_dispatcher.load_funclist(config.funclist_path);  // 启动期先保证 XML 可用，再允许任务进入。
    if (!funclist_result) {
        logger.error("cannot load LBM funclist: status=" +
                     std::string(oxmbs::lbm::funclist_status_name(funclist_result.status)) +
                     " error=" + funclist_result.error);
        return 1;  // XML 路由不完整时不能启动 Worker，避免低频请求被默认 Mock 或未知函数处理。
    }
    logger.info("LBM funclist loaded; path=" + config.funclist_path.string() +
                " routes=" + std::to_string(lbm_dispatcher.loader().configured_route_count()));

    oxmbs::ipc::IpcQueueManager queue_manager;  // 双队列仍由父进程独占创建和删除，LBM 不改变 IPC 所有权。
    if (queue_manager.open(default_ipc_queue_options(), error) != oxmbs::ipc::IpcResult::Ok) {
        logger.error("cannot create IPC queues: " + error);
        return 1;
    }

    oxmbs::ipc::TaskCorrelationRegistry correlation_registry;
    oxmbs::worker::QueryWorkerRuntime worker_runtime(  // Worker 收到任务后才经 Dispatcher 首次触发 dlopen/dlsym。
        queue_manager.request_queue(), queue_manager.reply_queue(), {},
        &oxmbs::lbm::LbmTaskDispatcher::handle, &lbm_dispatcher, &logger);  // 仅在 Worker 内按 XML 懒加载 LBM；主进程继续只负责 IPC、监管和回包。
    oxapp::process::WorkerSupervisor supervisor(
        config.query_worker_count, &oxmbs::worker::query_worker_runtime_entry, &worker_runtime);
    if (!supervisor.start()) {  // fork/启动失败时不能留下已经创建的 System V 队列。
        logger.error("cannot start query worker supervisor");
        std::string close_error;
        queue_manager.close(close_error);  // 失败路径仍由主进程关闭双队列，避免 Worker 或内核残留资源。
        return 1;
    }

    oxmbs::worker::ReplyCollector reply_collector(
        queue_manager.reply_queue(), correlation_registry, {}, nullptr, nullptr, nullptr, nullptr, &logger);
    std::atomic<int> reply_collector_exit_code{-1};
    std::thread reply_thread([&reply_collector, &reply_collector_exit_code] {  // Reply 接收单独阻塞，主线程仍可巡检 Worker 和处理超时。
        reply_collector_exit_code.store(reply_collector.run(), std::memory_order_release);  // Reply 接收只在独立低频线程阻塞，不占用 WorkFunc 或主监控循环。
    });

    logger.info("query worker runtime started; request_queue_id=" +
                std::to_string(queue_manager.request_queue().id()) + " reply_queue_id=" +
                std::to_string(queue_manager.reply_queue().id()));

    int exit_code = 0;
    while (stop_requested == 0) {
        supervisor.monitor_once();  // #2 非阻塞巡检并按需重启异常 Worker。
        reply_collector.expire_pending();  // #5 steady_clock 超时由主进程统一清理；未来可将 snapshot 交给 HP-Socket 超时回包。
        if (reply_collector_exit_code.load(std::memory_order_acquire) != -1) {
            logger.error("reply collector stopped unexpectedly");
            exit_code = 1;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{50});  // 主进程只轮询监管与超时，不执行查询业务。
    }

    logger.info("service stopping");
    if (!supervisor.stop(std::chrono::milliseconds{config.shutdown_timeout_ms})) {
        logger.error("one or more query workers did not stop cleanly");
        exit_code = 1;
    }
    std::string close_error;
    if (queue_manager.close(close_error) != oxmbs::ipc::IpcResult::Ok) {
        logger.error("IPC queue cleanup failed: " + close_error);
        exit_code = 1;
    }
    if (reply_thread.joinable()) {
        reply_thread.join();  // Reply Queue 删除会以 QueueRemoved 唤醒 collect_one()，随后线程正常退出。
    }
    if (reply_collector_exit_code.load(std::memory_order_acquire) != 0) {
        logger.error("reply collector exited with failure");
        exit_code = 1;
    }
    logger.info("service stopped");
    return exit_code;
}
