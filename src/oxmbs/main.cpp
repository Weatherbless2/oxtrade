#include "oxmbs/config/mbs_config.h"
#include "oxmbs/ipc/ipc_queue_manager.h"
#include "oxmbs/logging/run_logger.h"
#include "oxmbs/lbm/lbm_task_dispatcher.h"
#include "oxmbs/network/hp_socket_bridge.h"
#include "oxmbs/network/hp_socket_server.h"
#include "oxmbs/runtime/system_info.h"
#include "oxmbs/routing/message_route_policy.h"
#include "oxmbs/routing/workfunc_router.h"
#include "oxmbs/service/qmt_service_router.h"
#include "oxmbs/storage/postgres_pool.h"
#include "oxmbs/storage/qmt_repository.h"
#include "oxmbs/worker/query_worker_runtime.h"
#include "oxmbs/worker/reply_collector.h"

#include "oxapp/process/worker_supervisor.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_set>

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

bool open_worker_database(void* context, std::string& error) {
    if (context == nullptr) {
        error = "worker PostgreSQL pool context is null";
        return false;
    }
    auto* pool = static_cast<oxmbs::storage::PostgresConnectionPool*>(context);
    oxmbs::storage::PostgresFailure failure;
    if (!pool->open(failure)) {
        error = oxmbs::storage::postgres_status_name(failure.status);
        return false;
    }
    return true;
}

void load_database_conninfo_from_environment(oxmbs::storage::PostgresPoolOptions& options) {
    const char* conninfo = std::getenv("OXMBS_DATABASE_URL");
    if (conninfo == nullptr || *conninfo == '\0') {
        conninfo = std::getenv("DATABASE_URL");
    }
    if (conninfo != nullptr && *conninfo != '\0') {
        options.connection.conninfo = conninfo;  // 凭据只从运行环境注入，不进入仓库文件或日志。
    }
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

    oxmbs::storage::PostgresPoolOptions database_options;
    load_database_conninfo_from_environment(database_options);
    oxmbs::storage::PostgresConnectionPool database_pool(database_options);  // fork 前保持 closed，避免复制 libpq 连接句柄。
    oxmbs::storage::PostgresConnectionPool database_probe(database_options);
    oxmbs::storage::PostgresFailure database_failure;
    if (!database_probe.open(database_failure)) {
        logger.error("PostgreSQL preflight failed status=" +
                     std::string(oxmbs::storage::postgres_status_name(database_failure.status)));
        return 1;
    }
    database_probe.close();  // 只验证当前运行环境可连接；真正业务连接在每个 Worker fork 后独立建立。
    logger.info("PostgreSQL preflight passed; worker connections will open after fork");

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
    oxmbs::storage::QmtRepository qmt_repository(database_pool);
    oxmbs::service::QmtRepositoryAdapter qmt_repository_adapter(qmt_repository);
    oxmbs::service::QmtServiceRouterOptions qmt_options;
    qmt_options.routes = {
        {2, 1, oxmbs::service::QmtOperation::InstrumentSearch},
        {2, 2, oxmbs::service::QmtOperation::CalendarPage},
        {2, 3, oxmbs::service::QmtOperation::QuoteLatest},
    };
    oxmbs::service::QmtServiceRouter qmt_service(qmt_repository_adapter, qmt_options);
    oxmbs::worker::QueryWorkerRuntime worker_runtime(  // Worker 收到任务后才经 Dispatcher 首次触发 dlopen/dlsym。
        queue_manager.request_queue(), queue_manager.reply_queue(), {},
        &oxmbs::service::QmtServiceRouter::handle, &qmt_service, &logger,
        &open_worker_database, &database_pool);  // QMT Service 在 Worker 内执行，连接池也在 fork 后独立打开。
    oxapp::process::WorkerSupervisor supervisor(
        config.query_worker_count, &oxmbs::worker::query_worker_runtime_entry, &worker_runtime);
    if (!supervisor.start()) {  // fork/启动失败时不能留下已经创建的 System V 队列。
        logger.error("cannot start query worker supervisor");
        std::string close_error;
        queue_manager.close(close_error);  // 失败路径仍由主进程关闭双队列，避免 Worker 或内核残留资源。
        return 1;
    }

    oxmbs::routing::WorkFuncRouter workfunc_router(
        queue_manager, correlation_registry,
        oxmbs::routing::MessageRoutePolicy(std::unordered_set<std::uint32_t>{2}),
        {}, &logger);
    oxmbs::network::HpSocketBridge network_bridge(nullptr, workfunc_router, &logger);
    oxmbs::worker::ReplyCollector reply_collector(
        queue_manager.reply_queue(), correlation_registry, {},
        &oxmbs::network::HpSocketBridge::on_response, &network_bridge,
        &oxmbs::network::HpSocketBridge::on_timeout, &network_bridge, &logger);
    std::atomic<int> reply_collector_exit_code{-1};
    std::thread reply_thread([&reply_collector, &reply_collector_exit_code] {  // Reply 接收单独阻塞，主线程仍可巡检 Worker 和处理超时。
        reply_collector_exit_code.store(reply_collector.run(), std::memory_order_release);  // Reply 接收只在独立低频线程阻塞，不占用 WorkFunc 或主监控循环。
    });

    logger.info("query worker runtime started; request_queue_id=" +
                std::to_string(queue_manager.request_queue().id()) + " reply_queue_id=" +
                std::to_string(queue_manager.reply_queue().id()));

    oxmbs::network::HpSocketServerOptions network_options;
    network_options.bind_host = config.listen_host;
    network_options.port = config.listen_port;
    network_options.logger = &logger;
    network_options.frame_sink = &oxmbs::network::HpSocketBridge::on_frame;
    network_options.frame_sink_context = &network_bridge;
    network_options.connection_closed_sink = &oxmbs::network::HpSocketBridge::on_connection_closed;
    network_options.connection_closed_context = &network_bridge;
    oxmbs::network::HpSocketServer hp_socket_server(network_options);
    network_bridge.attach_server(hp_socket_server);
    if (!hp_socket_server.start(error)) {  // 网络监听最后启动；前置 IPC/Worker 未就绪时不会接受客户端请求。
        logger.error("cannot start HP-Socket server: " + error);
        std::string stop_error;
        supervisor.stop(std::chrono::milliseconds{config.shutdown_timeout_ms});
        queue_manager.close(stop_error);
        if (reply_thread.joinable()) {
            reply_thread.join();
        }
        return 1;
    }
    logger.info("HP-Socket server runtime started; listen=" + config.listen_host + ":" +
                std::to_string(config.listen_port));

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
    std::string network_stop_error;
    if (!hp_socket_server.stop(network_stop_error)) {
        logger.error("HP-Socket server stop failed: " + network_stop_error);
        exit_code = 1;
    }
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
