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
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() /
                           ("oxmbs_lbm_" + name + "_" + std::to_string(suffix));
    std::filesystem::create_directories(directory);
    return directory;
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream stream(path, std::ios::binary);
    assert(stream);
    stream << content;
    assert(stream.good());
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
    assert(oxmbs::ipc::encode_task(task, frame, error) == oxmbs::ipc::TaskProtocolStatus::Ok);
    assert(queue.send(kRequestMessageType, frame.data(), frame.size(), false, error) == oxmbs::ipc::IpcResult::Ok);
}

void test_registry_validates_and_preserves_last_good_mapping() {
    const auto directory = temporary_directory("registry");
    const auto valid_path = directory / "valid.xml";
    write_file(valid_path,
               "<funclist>\n"
               "  <function msg_type=\"9001\" func_id=\"41\" library=\"modules/lbm_test.so\" symbol=\"entry\" />\n"
               "</funclist>\n");

    oxmbs::lbm::FunclistRegistry registry;
    assert(registry.load_from_file(valid_path));
    const auto* route = registry.find(kMessageType, 41);
    assert(route != nullptr);
    assert(route->library_path == directory / "modules/lbm_test.so");  // XML 相对库路径只以 XML 所在目录解释。
    assert(route->symbol == "entry");

    const auto duplicate_path = directory / "duplicate.xml";
    write_file(duplicate_path,
               "<funclist>\n"
               "  <function msg_type=\"9001\" func_id=\"41\" library=\"a.so\" symbol=\"a\" />\n"
               "  <function msg_type=\"9001\" func_id=\"41\" library=\"b.so\" symbol=\"b\" />\n"
               "</funclist>\n");
    const auto duplicate = registry.load_from_file(duplicate_path);
    assert(duplicate.status == oxmbs::lbm::FunclistStatus::DuplicateRoute);
    assert(registry.size() == 1 && registry.find(kMessageType, 41) != nullptr);  // 错误重载不能破坏上一份已生效路由。

    const auto invalid_root_path = directory / "invalid_root.xml";
    write_file(invalid_root_path, "<funclist mode=\"unsafe\"></funclist>\n");
    assert(registry.load_from_file(invalid_root_path).status == oxmbs::lbm::FunclistStatus::InvalidRoot);
    std::filesystem::remove_all(directory);
}

void test_loader_lazily_caches_modules_and_reports_failures() {
    const auto directory = temporary_directory("loader");
    const auto funclist = write_valid_funclist(directory);
    oxmbs::lbm::LbmModuleLoader loader;

    assert(loader.execute(make_task(41)).module_status == oxmbs::lbm::LbmModuleStatus::ConfigurationNotLoaded);
    assert(loader.load_funclist(funclist));
    assert(loader.configured_route_count() == 5);
    assert(loader.loaded_library_count() == 0);  // 读取 XML 不得抢先 dlopen，模块必须由首个实际任务触发。

    const auto normal = loader.execute(make_task(41));
    assert(normal.module_status == oxmbs::lbm::LbmModuleStatus::Ok);
    assert(normal.response_status == oxmbs::ipc::IpcResponseStatus::Ok);
    assert(std::string(normal.payload.begin(), normal.payload.end()) == "mock-lbm-ok");
    assert(loader.loaded_library_count() == 1 && loader.cached_function_count() == 1);

    const auto repeated = loader.execute(make_task(41));
    assert(repeated.module_status == oxmbs::lbm::LbmModuleStatus::Ok);
    assert(loader.loaded_library_count() == 1 && loader.cached_function_count() == 1);  // 重复路由复用同一 .so 句柄与函数指针。

    const auto rejected = loader.execute(make_task(42));
    assert(rejected.module_status == oxmbs::lbm::LbmModuleStatus::Ok);
    assert(rejected.response_status == oxmbs::ipc::IpcResponseStatus::Rejected);
    assert(loader.cached_function_count() == 2);

    assert(loader.execute(make_task(43)).module_status == oxmbs::lbm::LbmModuleStatus::InvalidModuleResponse);
    assert(loader.execute(make_task(44)).module_status == oxmbs::lbm::LbmModuleStatus::InvocationFailed);
    assert(loader.execute(make_task(45)).module_status == oxmbs::lbm::LbmModuleStatus::SymbolNotFound);
    assert(loader.execute(make_task(99)).module_status == oxmbs::lbm::LbmModuleStatus::RouteNotFound);

    const auto missing_library = directory / "missing_library.xml";
    write_file(missing_library,
               "<funclist><function msg_type=\"9001\" func_id=\"41\" library=\"missing.so\" symbol=\"entry\" /></funclist>");
    oxmbs::lbm::LbmModuleLoader missing_loader;
    assert(missing_loader.load_funclist(missing_library));
    assert(missing_loader.execute(make_task(41)).module_status == oxmbs::lbm::LbmModuleStatus::LibraryLoadFailed);
    std::filesystem::remove_all(directory);
}

void test_supervised_worker_dispatches_to_lbm_and_writes_response() {
    const auto directory = temporary_directory("worker");
    const auto funclist = write_valid_funclist(directory);
    oxmbs::lbm::LbmTaskDispatcher dispatcher;
    assert(dispatcher.load_funclist(funclist));

    oxmbs::ipc::IpcQueueManager manager;
    std::string error;
    assert(manager.open(queue_options(), error) == oxmbs::ipc::IpcResult::Ok);
    oxmbs::worker::QueryWorkerRuntime runtime(
        manager.request_queue(), manager.reply_queue(), worker_options(),
        &oxmbs::lbm::LbmTaskDispatcher::handle, &dispatcher);  // Dispatcher 在 fork 前只加载 XML，子进程收到任务后才首次 dlopen。
    oxapp::process::WorkerSupervisor supervisor(1, &oxmbs::worker::query_worker_runtime_entry, &runtime);
    assert(supervisor.start());

    const auto task = make_task(41);
    send_task(manager.request_queue(), task);
    std::vector<std::uint8_t> buffer(4096);
    const auto received = manager.reply_queue().receive(kReplyMessageType, buffer.data(), buffer.size(), false);
    assert(received);
    const auto response = oxmbs::ipc::decode_response(buffer.data(), received.size);
    assert(response);
    assert(response.message.task_seq == task.task_seq && response.message.session_id == task.session_id);
    assert(response.message.status == oxmbs::ipc::IpcResponseStatus::Ok);
    assert(std::string(response.message.payload.begin(), response.message.payload.end()) == "mock-lbm-ok");

    assert(supervisor.stop(std::chrono::milliseconds{500}));
    assert(manager.close(error) == oxmbs::ipc::IpcResult::Ok);  // 父进程仍是唯一 System V 队列删除者。
    std::filesystem::remove_all(directory);
}

}  // namespace

int main() {
    test_registry_validates_and_preserves_last_good_mapping();
    test_loader_lazily_caches_modules_and_reports_failures();
    test_supervised_worker_dispatches_to_lbm_and_writes_response();
    return 0;
}
