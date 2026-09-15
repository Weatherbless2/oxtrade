#include "oxapp/process/worker_supervisor.h"
#include "oxmbs/ipc/ipc_queue_manager.h"
#include "oxmbs/ipc/task_protocol.h"
#include "oxmbs/lbm/funclist_registry.h"
#include "oxmbs/lbm/lbm_module_loader.h"
#include "oxmbs/lbm/lbm_task_dispatcher.h"
#include "oxmbs/worker/query_worker_runtime.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kMessageType = 9001;  // 仅为 #8 mock 路由测试，不能作为正式 PackMessage 枚举。
constexpr long kRequestMessageType = 71;  // System V mtype 与上面的业务 msg_type 严格分离。
constexpr long kReplyMessageType = 72;

std::filesystem::path temporary_directory(const std::string& name) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();  // 用单调时钟生成隔离名，避免多个测试共享 XML/so 文件。
    const auto directory = std::filesystem::temp_directory_path() /
                           ("oxmbs_lbm_" + name + "_" + std::to_string(suffix));
    std::filesystem::create_directories(directory);  // 测试自己创建运行目录，结束时由该测试整体删除。
    return directory;  // 返回路径供 XML、队列和动态库测试共同使用。
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream stream(path, std::ios::binary);  // 直接构造文件输入，模拟部署目录中的 ini/XML/路由文件。
    assert(stream);  // 打不开说明测试环境准备失败，后续断言没有业务意义。
    stream << content;  // 写入完整字节文本，不让测试依赖真实配置文件。
    assert(stream.good());  // 确认写入成功，否则不能把失败归因到 Loader。
}

std::string xml_entry(std::uint32_t function_id, const std::string& symbol) {
    return "    <function msg_type=\"" + std::to_string(kMessageType) +
           "\" func_id=\"" + std::to_string(function_id) +
           "\" library=\"" + std::string(OXMBS_LBM_TEST_LIBRARY) +
           "\" symbol=\"" + symbol + "\" />\n";
}

std::filesystem::path write_valid_funclist(const std::filesystem::path& directory) {
    const auto path = directory / "funclist.xml";
    write_file(path,
               "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<funclist>\n" +
                   xml_entry(41, "oxmbs_lbm_test_execute") +
                   xml_entry(42, "oxmbs_lbm_test_reject") +
                   xml_entry(43, "oxmbs_lbm_test_invalid_response") +
                   xml_entry(44, "oxmbs_lbm_test_return_failure") +
                   xml_entry(45, "oxmbs_lbm_test_missing_symbol") +
               "</funclist>\n");
    return path;
}

std::uint64_t deadline_after(std::chrono::milliseconds duration) {
    const auto now = std::chrono::system_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>((now + duration).time_since_epoch()).count());
}

oxmbs::ipc::IpcTaskMsg make_task(std::uint32_t function_id) {
    oxmbs::ipc::IpcTaskMsg task;
    task.task_seq = 1000 + function_id;
    task.session_id = 8000 + function_id;
    task.deadline_unix_ms = deadline_after(std::chrono::seconds{2});
    task.msg_type = kMessageType;
    task.func_id = function_id;
    task.payload = {'p', 'i', 'n', 'g'};
    return task;
}

oxmbs::ipc::IpcQueueOptions queue_options() {
    oxmbs::ipc::IpcQueueOptions options;
    options.request.max_bytes = 4096;
    options.reply.max_bytes = 4096;
    options.request.permissions = 0600;
    options.reply.permissions = 0600;
    return options;
}

struct BuiltinInvocation {
    int call_count{0};
};

oxmbs::worker::QueryExecutionResult builtin_handler(const oxmbs::ipc::IpcTaskMsg&, void* context) {
    auto* invocation = static_cast<BuiltinInvocation*>(context);
    assert(invocation != nullptr);  // 测试显式传入上下文，证明 Dispatcher 不会生成或猜测业务对象。
    ++invocation->call_count;
    oxmbs::worker::QueryExecutionResult result;
    result.status = oxmbs::ipc::IpcResponseStatus::Ok;
    constexpr char kPayload[] = "builtin-bridge";
    result.payload.assign(kPayload, kPayload + sizeof(kPayload) - 1);
    return result;
}

oxmbs::worker::QueryWorkerRuntimeOptions worker_options() {
    oxmbs::worker::QueryWorkerRuntimeOptions options;
    options.protocol_limits.max_frame_bytes = 4096;
    options.request_message_type = kRequestMessageType;
    options.reply_message_type = kReplyMessageType;
    return options;
}

void send_task(oxmbs::ipc::SystemVMessageQueue& queue, const oxmbs::ipc::IpcTaskMsg& task) {
    oxmbs::ipc::IpcPayload frame;
    std::string error;
    assert(oxmbs::ipc::encode_task(task, frame, error) == oxmbs::ipc::TaskProtocolStatus::Ok);  // 先把结构化任务编码成 IPC 字节帧，验证 LBM 前的协议边界。
    assert(queue.send(kRequestMessageType, frame.data(), frame.size(), false, error) == oxmbs::ipc::IpcResult::Ok);  // 父进程把请求放入 Request Queue，Worker 随后阻塞取走。
}

void test_registry_validates_and_preserves_last_good_mapping() {
    const auto directory = temporary_directory("registry");
    const auto valid_path = directory / "valid.xml";
    write_file(valid_path,
               "<funclist>\n"
               "  <function msg_type=\"9001\" func_id=\"41\" library=\"modules/lbm_test.so\" symbol=\"entry\" />\n"
               "</funclist>\n");

    oxmbs::lbm::FunclistRegistry registry;
    assert(registry.load_from_file(valid_path));  // 合法 XML 才能成为活动路由表。
    const auto* route = registry.find(kMessageType, 41);  // 用任务实际的两个编号查询，不依赖 XML 顺序。
    assert(route != nullptr);  // 找不到说明路由键构建或解析失败。
    assert(route->library_path == directory / "modules/lbm_test.so");  // 相对库路径只以 XML 所在目录解释，换 cwd 也不会漂移。
    assert(route->symbol == "entry");  // 保存导出函数名，暂时不 dlopen，验证启动期懒加载边界。

    const auto duplicate_path = directory / "duplicate.xml";
    write_file(duplicate_path,
               "<funclist>\n"
               "  <function msg_type=\"9001\" func_id=\"41\" library=\"a.so\" symbol=\"a\" />\n"
               "  <function msg_type=\"9001\" func_id=\"41\" library=\"b.so\" symbol=\"b\" />\n"
               "</funclist>\n");
    const auto duplicate = registry.load_from_file(duplicate_path);  // 主动制造重复二元键，模拟配置人员复制路由的错误。
    assert(duplicate.status == oxmbs::lbm::FunclistStatus::DuplicateRoute);  // 后一个条目不能覆盖前一个条目。
    assert(registry.size() == 1 && registry.find(kMessageType, 41) != nullptr);  // 错误重载不能破坏上一份已生效路由。

    const auto invalid_root_path = directory / "invalid_root.xml";
    write_file(invalid_root_path, "<funclist mode=\"unsafe\"></funclist>\n");
    assert(registry.load_from_file(invalid_root_path).status == oxmbs::lbm::FunclistStatus::InvalidRoot);  // 根节点带额外属性时必须在启动阶段拒绝。
    std::filesystem::remove_all(directory);
}

void test_loader_lazily_caches_modules_and_reports_failures() {
    const auto directory = temporary_directory("loader");
    const auto funclist = write_valid_funclist(directory);
    oxmbs::lbm::LbmModuleLoader loader;

    assert(loader.execute(make_task(41)).module_status == oxmbs::lbm::LbmModuleStatus::ConfigurationNotLoaded);  // 故意跳过配置，确认 Loader 不会盲目加载任意模块。
    assert(loader.load_funclist(funclist));  // 先加载路由白名单，后续任务才有资格进入动态加载。
    assert(loader.configured_route_count() == 5);  // 五条路由都应进入候选表。
    assert(loader.loaded_library_count() == 0);  // 读取 XML 不得抢先 dlopen，模块必须由首个实际任务触发。

    const auto normal = loader.execute(make_task(41));
    assert(normal.module_status == oxmbs::lbm::LbmModuleStatus::Ok);  // 首次命中应完成 dlopen、dlsym 和模块调用。
    assert(normal.response_status == oxmbs::ipc::IpcResponseStatus::Ok);  // 模块 Ok 被翻译成统一 IPC 成功状态。
    assert(std::string(normal.payload.begin(), normal.payload.end()) == "mock-lbm-ok");  // 返回内容来自 Worker 提供的输出缓冲区。
    assert(loader.loaded_library_count() == 1 && loader.cached_function_count() == 1);  // 首次调用后只产生一个库句柄和一个函数缓存。

    const auto repeated = loader.execute(make_task(41));
    assert(repeated.module_status == oxmbs::lbm::LbmModuleStatus::Ok);  // 第二次调用验证缓存函数地址仍可复用。
    assert(loader.loaded_library_count() == 1 && loader.cached_function_count() == 1);  // 重复路由复用同一 .so 句柄与函数指针。

    const auto rejected = loader.execute(make_task(42));
    assert(rejected.module_status == oxmbs::lbm::LbmModuleStatus::Ok);  // 模块自身执行成功，但业务状态可以是 Rejected。
    assert(rejected.response_status == oxmbs::ipc::IpcResponseStatus::Rejected);  // 拒绝不是动态加载失败，要保留业务语义。
    assert(loader.cached_function_count() == 2);  // 同一 .so 的第二个符号加入函数缓存，共享一个库句柄。

    assert(loader.execute(make_task(43)).module_status == oxmbs::lbm::LbmModuleStatus::InvalidModuleResponse);  // 故意伪造 ABI 版本，确认 Loader 拒绝不可信结果。
    assert(loader.execute(make_task(44)).module_status == oxmbs::lbm::LbmModuleStatus::InvocationFailed);  // 故意返回非零，模拟模块内部业务/系统失败。
    assert(loader.execute(make_task(45)).module_status == oxmbs::lbm::LbmModuleStatus::SymbolNotFound);  // 路由存在但导出函数不存在，不能调用空地址。
    assert(loader.execute(make_task(99)).module_status == oxmbs::lbm::LbmModuleStatus::RouteNotFound);  // 未配置编号必须拒绝，不得猜测默认函数。

    const auto missing_library = directory / "missing_library.xml";
    write_file(missing_library,
               "<funclist><function msg_type=\"9001\" func_id=\"41\" library=\"missing.so\" symbol=\"entry\" /></funclist>");
    oxmbs::lbm::LbmModuleLoader missing_loader;
    assert(missing_loader.load_funclist(missing_library));  // XML 结构合法，但把库文件故意指向不存在路径。
    assert(missing_loader.execute(make_task(41)).module_status == oxmbs::lbm::LbmModuleStatus::LibraryLoadFailed);  // 失败应在首次实际命中时报告，而不是启动时误报。
    std::filesystem::remove_all(directory);
}

void test_dispatcher_prefers_xml_and_only_uses_explicit_builtin_routes() {
    const auto directory = temporary_directory("dispatcher");
    const auto funclist = write_valid_funclist(directory);
    oxmbs::lbm::LbmTaskDispatcher dispatcher;
    BuiltinInvocation invocation;

    dispatcher.register_builtin_route(kMessageType, 46, &builtin_handler, &invocation);
    assert(dispatcher.builtin_route_count() == 1);  // 迁移桥接必须是逐个二元键登记，不能覆盖所有未配置请求。
    std::string error;
    assert(!dispatcher.initialize_worker(error));  // funclist 未加载前，即使有桥接也不允许启动 Worker。
    assert(!error.empty());
    const auto before_configuration = dispatcher.execute(make_task(46));
    assert(before_configuration.status == oxmbs::ipc::IpcResponseStatus::Failed);
    assert(invocation.call_count == 0);  // 启动顺序错误时不能绕过 XML 配置门禁直接调用业务 Service。

    assert(dispatcher.load_funclist(funclist));
    assert(dispatcher.initialize_worker(error));
    assert(error.empty());
    assert(dispatcher.loader().route_configured(kMessageType, 41));
    assert(!dispatcher.loader().route_configured(kMessageType, 46));

    const auto bridged = dispatcher.execute(make_task(46));
    assert(bridged.status == oxmbs::ipc::IpcResponseStatus::Ok);
    assert(std::string(bridged.payload.begin(), bridged.payload.end()) == "builtin-bridge");
    assert(invocation.call_count == 1);

    const auto unregistered = dispatcher.execute(make_task(99));
    assert(unregistered.status == oxmbs::ipc::IpcResponseStatus::Rejected);
    assert(invocation.call_count == 1);  // 未登记路由仍拒绝，不能被 bridge 误接收。

    dispatcher.register_builtin_route(kMessageType, 41, &builtin_handler, &invocation);
    const auto xml_route = dispatcher.execute(make_task(41));
    assert(xml_route.status == oxmbs::ipc::IpcResponseStatus::Ok);
    assert(std::string(xml_route.payload.begin(), xml_route.payload.end()) == "mock-lbm-ok");
    assert(invocation.call_count == 1);  // XML 正式路由优先，未来 lbm_qmt.so 上线后同键桥接不会生效。
    std::filesystem::remove_all(directory);
}

void test_supervised_worker_dispatches_to_lbm_and_writes_response() {
    const auto directory = temporary_directory("worker");
    const auto funclist = write_valid_funclist(directory);
    oxmbs::lbm::LbmTaskDispatcher dispatcher;
    assert(dispatcher.load_funclist(funclist));  // 父进程只先完成 XML 校验，动态库由 fork 后 Worker 首次任务触发。

    oxmbs::ipc::IpcQueueManager manager;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);  // 创建真实双向 System V 队列，验证跨进程链路而非直接函数调用。
    oxmbs::worker::QueryWorkerRuntime runtime(
        manager.request_queue(), manager.reply_queue(), worker_options(),
        &oxmbs::lbm::LbmTaskDispatcher::handle, &dispatcher);  // Dispatcher 在 fork 前只加载 XML，子进程收到任务后才首次 dlopen。
    oxapp::process::WorkerSupervisor supervisor(1, &oxmbs::worker::query_worker_runtime_entry, &runtime);
    assert(supervisor.start());  // fork Worker；子进程接收 Request 后执行 Dispatcher。

    const auto task = make_task(41);
    send_task(manager.request_queue(), task);
    std::vector<std::uint8_t> buffer(4096);
    const auto received = manager.reply_queue().receive(kReplyMessageType, buffer.data(), buffer.size(), false);
    assert(received);  // 主进程阻塞等待 Reply Queue，收到的必须是对应 mtype 的完整帧。
    const auto response = oxmbs::ipc::decode_response(buffer.data(), received.size);  // 按协议还原 Worker 发送的状态和字节正文。
    assert(response);  // 解码失败说明 IPC 长度/布局或 Worker 编码有问题。
    assert(response.message.task_seq == task.task_seq && response.message.session_id == task.session_id);  // 关联字段必须原样往返，防止串单。
    assert(response.message.status == oxmbs::ipc::IpcResponseStatus::Ok);  // 端到端状态应保持模块成功语义。
    assert(std::string(response.message.payload.begin(), response.message.payload.end()) == "mock-lbm-ok");  // 最终回包正文来自动态模块。

    assert(supervisor.stop(std::chrono::milliseconds{500}));  // 先停止 Worker，避免子进程继续访问即将删除的队列。
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);  // 父进程仍是唯一 System V 队列删除者，测试结束不留内核资源。
    std::filesystem::remove_all(directory);
}

}  // namespace

int main() {
    test_registry_validates_and_preserves_last_good_mapping();
    test_loader_lazily_caches_modules_and_reports_failures();
    test_dispatcher_prefers_xml_and_only_uses_explicit_builtin_routes();
    test_supervised_worker_dispatches_to_lbm_and_writes_response();
    return 0;
}
