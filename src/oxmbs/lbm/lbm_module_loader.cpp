#include "oxmbs/lbm/lbm_module_loader.h"

#include <cstring>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace oxmbs::lbm {
namespace {

ipc::IpcResponseStatus failure_status(LbmModuleStatus status) noexcept {
    return status == LbmModuleStatus::RouteNotFound ? ipc::IpcResponseStatus::Rejected
                                                     : ipc::IpcResponseStatus::Failed;
}

}  // namespace

const char* lbm_module_status_name(LbmModuleStatus status) noexcept {
    switch (status) {
    case LbmModuleStatus::Ok:
        return "Ok";
    case LbmModuleStatus::ConfigurationNotLoaded:
        return "ConfigurationNotLoaded";
    case LbmModuleStatus::RouteNotFound:
        return "RouteNotFound";
    case LbmModuleStatus::DynamicLoadingUnsupported:
        return "DynamicLoadingUnsupported";
    case LbmModuleStatus::LibraryLoadFailed:
        return "LibraryLoadFailed";
    case LbmModuleStatus::SymbolNotFound:
        return "SymbolNotFound";
    case LbmModuleStatus::InvocationFailed:
        return "InvocationFailed";
    case LbmModuleStatus::InvalidModuleResponse:
        return "InvalidModuleResponse";
    }
    return "Unknown";
}

LbmModuleLoader::LbmModuleLoader(LbmModuleLoaderOptions options)
    : options_(options) {
}

LbmModuleLoader::~LbmModuleLoader() {
    std::lock_guard<std::mutex> lock(cache_mutex_);  // 先封住缓存，避免析构期间另一个线程还在取函数地址。
#if defined(__unix__) || defined(__APPLE__)
    for (auto& [path, library] : libraries_) {
        (void)path;
        if (library.native_handle != nullptr) {
            ::dlclose(library.native_handle);  // LBM 句柄只属于 Loader，Worker 停止析构时统一释放引用。
        }
    }
#endif
    libraries_.clear();  // 句柄全部关闭后再丢弃 map，避免失去最后的释放记录。
}

FunclistLoadResult LbmModuleLoader::load_funclist(const std::filesystem::path& path) {
    const auto result = registry_.load_from_file(path);  // Registry 先完整校验并原子替换路由表。
    if (result) {
        configuration_loaded_ = true;  // 只有成功配置才放行 execute，失败不会误开启半成品 Loader。
    }
    return result;
}

LbmModuleResult LbmModuleLoader::execute(const ipc::IpcTaskMsg& task) {
    LbmModuleResult result;
    if (!configuration_loaded_) {  // 防止任务在启动顺序错误时直接触发未知动态加载。
        result.module_status = LbmModuleStatus::ConfigurationNotLoaded;
        result.response_status = failure_status(result.module_status);
        result.error = "funclist configuration has not been loaded";
        result.payload = error_payload(result.error);
        return result;
    }
    const auto* route = registry_.find(task.msg_type, task.func_id);  // 输入编号来自已解码 IPC 任务，必须同时命中 XML 白名单。
    if (route == nullptr) {
        result.module_status = LbmModuleStatus::RouteNotFound;
        result.response_status = failure_status(result.module_status);
        result.error = "no funclist route for msg_type=" + std::to_string(task.msg_type) +
                       " func_id=" + std::to_string(task.func_id);
        result.payload = error_payload(result.error);
        return result;
    }

    LbmEntryPoint entry_point{nullptr};  // 函数指针只在当前调用中借用，真正所有权仍属于缓存句柄。
    const auto resolved = resolve_entry_point(*route, entry_point);  // 这里可能执行一次 dlopen/dlsym，也可能直接命中缓存。
    if (!resolved) {
        return resolved;
    }

    ipc::IpcPayload response_storage(options_.max_response_payload_bytes);  // Worker 分配固定输出区，限制结果最终可进入 IPC 帧的最大字节数。
    LbmRequestView request;
    request.task_seq = task.task_seq;
    request.session_id = task.session_id;
    request.deadline_unix_ms = task.deadline_unix_ms;
    request.msg_type = task.msg_type;
    request.func_id = task.func_id;
    request.payload = task.payload.data();  // 只借给 .so 读取当前任务正文，模块不能保存该地址到调用返回之后。
    request.payload_size = task.payload.size();  // 长度是正文字节数，不包含 C++ 容器或字符串结束符。

    LbmResponseBuffer response;
    response.payload = response_storage.data();  // 输出地址由 Worker 预先决定，模块只能写这块内存。
    response.payload_capacity = response_storage.size();  // 把容量一起传入，模块写入前必须自行检查越界。
    const int entry_result = entry_point(&request, &response);  // 只跨边界传 POD 指针/长度，避免暴露 std::vector 等 C++ ABI。
    if (entry_result != 0) {
        result.module_status = LbmModuleStatus::InvocationFailed;
        result.response_status = ipc::IpcResponseStatus::Failed;
        result.error = "LBM entry point returned non-zero code " + std::to_string(entry_result) +
                       " symbol=" + route->symbol;
        result.payload = error_payload(result.error);
        return result;
    }
    if (response.abi_version != kLbmAbiVersion || !is_valid_lbm_response_code(response.status) ||
        response.payload != response_storage.data() ||
        response.payload_capacity != response_storage.size() ||
        response.payload_size > response_storage.size()) {  // 任一字段被模块改写都视为不可信，绝不按声称长度复制。
        result.module_status = LbmModuleStatus::InvalidModuleResponse;
        result.response_status = ipc::IpcResponseStatus::Failed;
        result.error = "LBM returned an invalid ABI version, response buffer or payload size";
        result.payload = error_payload(result.error);
        return result;
    }

    result.module_status = LbmModuleStatus::Ok;
    result.response_status = to_ipc_response_status(response.status);  // 把模块协议状态翻译为 Worker/IPC 共用的稳定状态码。
    result.payload.assign(response_storage.begin(), response_storage.begin() + response.payload_size);  // 深拷贝到 Loader 自己拥有的容器，返回后 .so 可安全卸载。
    return result;
}

bool LbmModuleLoader::configuration_loaded() const noexcept {
    return configuration_loaded_;
}

std::size_t LbmModuleLoader::configured_route_count() const noexcept {
    return registry_.size();
}

std::size_t LbmModuleLoader::loaded_library_count() const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    return libraries_.size();
}

std::size_t LbmModuleLoader::cached_function_count() const {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    std::size_t count{0};
    for (const auto& [path, library] : libraries_) {
        (void)path;
        count += library.functions.size();
    }
    return count;
}

LbmModuleResult LbmModuleLoader::resolve_entry_point(const FunctionRoute& route, LbmEntryPoint& entry_point) {
    std::lock_guard<std::mutex> lock(cache_mutex_);  // 同一 Worker 的并发任务串行初始化缓存，避免重复句柄和竞态函数指针。
#if defined(__unix__) || defined(__APPLE__)
    auto library_it = libraries_.find(route.library_path);
    if (library_it == libraries_.end()) {
        void* native_handle = ::dlopen(route.library_path.c_str(), RTLD_NOW | RTLD_LOCAL);  // 首次命中才加载；RTLD_LOCAL 防止模块符号污染其他库。
        if (native_handle == nullptr) {
            LbmModuleResult result;
            result.module_status = LbmModuleStatus::LibraryLoadFailed;
            result.response_status = failure_status(result.module_status);
            const char* detail = ::dlerror();  // 读取本线程最近一次 dlopen 错误，便于定位路径、依赖或权限问题。
            result.error = "cannot load LBM '" + route.library_path.string() + "'" +
                           (detail == nullptr ? std::string{} : ": " + std::string(detail));
            result.payload = error_payload(result.error);
            return result;
        }
        CachedLibrary library;
        library.native_handle = native_handle;
        library_it = libraries_.emplace(route.library_path, std::move(library)).first;  // 同一 .so 的多个 function 共享句柄，避免重复打开。
    }

    const auto function_it = library_it->second.functions.find(route.symbol);
    if (function_it != library_it->second.functions.end()) {
        entry_point = function_it->second;  // 已缓存函数地址可直接调用，不再次进入动态链接器。
        LbmModuleResult result;
        result.module_status = LbmModuleStatus::Ok;
        return result;
    }

    ::dlerror();  // 清空线程本地旧错误，确保后续诊断准确对应本次 dlsym。
    void* raw_symbol = ::dlsym(library_it->second.native_handle, route.symbol.c_str());  // 按 XML 指定的 extern "C" 名称查找导出入口。
    const char* detail = ::dlerror();  // dlsym 返回空或错误文本都代表不能安全调用。
    if (raw_symbol == nullptr || detail != nullptr) {
        LbmModuleResult result;
        result.module_status = LbmModuleStatus::SymbolNotFound;
        result.response_status = failure_status(result.module_status);
        result.error = "cannot resolve LBM symbol '" + route.symbol + "' from '" + route.library_path.string() + "'" +
                       (detail == nullptr ? std::string{} : ": " + std::string(detail));
        result.payload = error_payload(result.error);
        return result;
    }
    static_assert(sizeof(raw_symbol) == sizeof(entry_point), "platform must provide compatible dlsym pointer sizes");
    std::memcpy(&entry_point, &raw_symbol, sizeof(entry_point));  // 避免直接把 void* 转函数指针的编译器扩展写法。
    library_it->second.functions.emplace(route.symbol, entry_point);  // 只有确认地址有效后才进入缓存，失败不会留下假指针。
    LbmModuleResult result;
    result.module_status = LbmModuleStatus::Ok;
    return result;
#else
    (void)route;
    (void)entry_point;
    LbmModuleResult result;
    result.module_status = LbmModuleStatus::DynamicLoadingUnsupported;
    result.response_status = failure_status(result.module_status);
    result.error = "LBM dynamic loading is not implemented on this platform";
    result.payload = error_payload(result.error);
    return result;
#endif
}

ipc::IpcResponseStatus LbmModuleLoader::to_ipc_response_status(LbmResponseCode code) noexcept {
    switch (code) {
    case LbmResponseCode::Ok:
        return ipc::IpcResponseStatus::Ok;
    case LbmResponseCode::Rejected:
        return ipc::IpcResponseStatus::Rejected;
    case LbmResponseCode::TimedOut:
        return ipc::IpcResponseStatus::TimedOut;
    case LbmResponseCode::Failed:
        return ipc::IpcResponseStatus::Failed;
    }
    return ipc::IpcResponseStatus::Failed;
}

ipc::IpcPayload LbmModuleLoader::error_payload(const std::string& message) {
    return ipc::IpcPayload(message.begin(), message.end());
}

}  // namespace oxmbs::lbm
