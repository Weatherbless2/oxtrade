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
    std::lock_guard<std::mutex> lock(cache_mutex_);
#if defined(__unix__) || defined(__APPLE__)
    for (auto& [path, library] : libraries_) {
        (void)path;
        if (library.native_handle != nullptr) {
            ::dlclose(library.native_handle);  // LBM 句柄只属于 Loader，Worker 停止析构时统一释放引用。
        }
    }
#endif
    libraries_.clear();
}

FunclistLoadResult LbmModuleLoader::load_funclist(const std::filesystem::path& path) {
    const auto result = registry_.load_from_file(path);
    if (result) {
        configuration_loaded_ = true;
    }
    return result;
}

LbmModuleResult LbmModuleLoader::execute(const ipc::IpcTaskMsg& task) {
    LbmModuleResult result;
    if (!configuration_loaded_) {
        result.module_status = LbmModuleStatus::ConfigurationNotLoaded;
        result.response_status = failure_status(result.module_status);
        result.error = "funclist configuration has not been loaded";
        result.payload = error_payload(result.error);
        return result;
    }
    const auto* route = registry_.find(task.msg_type, task.func_id);
    if (route == nullptr) {
        result.module_status = LbmModuleStatus::RouteNotFound;
        result.response_status = failure_status(result.module_status);
        result.error = "no funclist route for msg_type=" + std::to_string(task.msg_type) +
                       " func_id=" + std::to_string(task.func_id);
        result.payload = error_payload(result.error);
        return result;
    }

    LbmEntryPoint entry_point{nullptr};
    const auto resolved = resolve_entry_point(*route, entry_point);
    if (!resolved) {
        return resolved;
    }

    ipc::IpcPayload response_storage(options_.max_response_payload_bytes);  // Worker 提供固定容量，LBM 不跨 .so 边界分配/释放内存。
    LbmRequestView request;
    request.task_seq = task.task_seq;
    request.session_id = task.session_id;
    request.deadline_unix_ms = task.deadline_unix_ms;
    request.msg_type = task.msg_type;
    request.func_id = task.func_id;
    request.payload = task.payload.data();
    request.payload_size = task.payload.size();

    LbmResponseBuffer response;
    response.payload = response_storage.data();
    response.payload_capacity = response_storage.size();
    const int entry_result = entry_point(&request, &response);  // 仅传递稳定 C ABI 值类型和借用 buffer，不暴露 std::vector 等 C++ ABI。
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
        response.payload_size > response_storage.size()) {
        result.module_status = LbmModuleStatus::InvalidModuleResponse;
        result.response_status = ipc::IpcResponseStatus::Failed;
        result.error = "LBM returned an invalid ABI version, response buffer or payload size";
        result.payload = error_payload(result.error);
        return result;
    }

    result.module_status = LbmModuleStatus::Ok;
    result.response_status = to_ipc_response_status(response.status);
    result.payload.assign(response_storage.begin(), response_storage.begin() + response.payload_size);  // 返回 Worker 自己拥有的字节，动态库不再参与结果生命周期。
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
    std::lock_guard<std::mutex> lock(cache_mutex_);  // 同一 Worker 多线程调用时只允许一个线程完成 dlopen/dlsym 缓存初始化。
#if defined(__unix__) || defined(__APPLE__)
    auto library_it = libraries_.find(route.library_path);
    if (library_it == libraries_.end()) {
        void* native_handle = ::dlopen(route.library_path.c_str(), RTLD_NOW | RTLD_LOCAL);  // 第一次命中路由才加载，避免启动时加载所有业务模块。
        if (native_handle == nullptr) {
            LbmModuleResult result;
            result.module_status = LbmModuleStatus::LibraryLoadFailed;
            result.response_status = failure_status(result.module_status);
            const char* detail = ::dlerror();
            result.error = "cannot load LBM '" + route.library_path.string() + "'" +
                           (detail == nullptr ? std::string{} : ": " + std::string(detail));
            result.payload = error_payload(result.error);
            return result;
        }
        CachedLibrary library;
        library.native_handle = native_handle;
        library_it = libraries_.emplace(route.library_path, std::move(library)).first;  // 同一 .so 的多个 function 路由共享一个句柄。
    }

    const auto function_it = library_it->second.functions.find(route.symbol);
    if (function_it != library_it->second.functions.end()) {
        entry_point = function_it->second;
        LbmModuleResult result;
        result.module_status = LbmModuleStatus::Ok;
        return result;
    }

    ::dlerror();  // 清空线程本地旧错误，确保后续诊断准确对应本次 dlsym。
    void* raw_symbol = ::dlsym(library_it->second.native_handle, route.symbol.c_str());
    const char* detail = ::dlerror();
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
    library_it->second.functions.emplace(route.symbol, entry_point);
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
