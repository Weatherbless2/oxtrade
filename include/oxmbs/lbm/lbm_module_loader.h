#pragma once

#include "oxmbs/ipc/task_protocol.h"
#include "oxmbs/lbm/funclist_registry.h"
#include "oxmbs/lbm/lbm_abi.h"

#include <cstddef>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

namespace oxmbs::lbm {

enum class LbmModuleStatus {
    Ok,
    ConfigurationNotLoaded,
    RouteNotFound,
    DynamicLoadingUnsupported,
    LibraryLoadFailed,
    SymbolNotFound,
    InvocationFailed,
    InvalidModuleResponse,
};

const char* lbm_module_status_name(LbmModuleStatus status) noexcept;

struct LbmModuleLoaderOptions {
    std::size_t max_response_payload_bytes{4032};  // 4096 最大帧减去 56 字节 v2 固定头，防止 LBM 结果到编码阶段才溢出。
};

struct LbmModuleResult {
    LbmModuleStatus module_status{LbmModuleStatus::InvocationFailed};
    ipc::IpcResponseStatus response_status{ipc::IpcResponseStatus::Failed};
    ipc::IpcPayload payload;
    std::string error;
    explicit operator bool() const noexcept { return module_status == LbmModuleStatus::Ok; }
};

class LbmModuleLoader {
public:
    explicit LbmModuleLoader(LbmModuleLoaderOptions options = {});
    ~LbmModuleLoader();

    LbmModuleLoader(const LbmModuleLoader&) = delete;
    LbmModuleLoader& operator=(const LbmModuleLoader&) = delete;

    FunclistLoadResult load_funclist(const std::filesystem::path& path);  // 只解析映射，不预加载动态库。
    LbmModuleResult execute(const ipc::IpcTaskMsg& task);

    bool configuration_loaded() const noexcept;
    std::size_t configured_route_count() const noexcept;
    std::size_t loaded_library_count() const;
    std::size_t cached_function_count() const;

private:
    struct CachedLibrary {
        void* native_handle{nullptr};  // 仅 Loader 拥有，析构时 dlclose。
        std::map<std::string, LbmEntryPoint> functions;
    };

    LbmModuleResult resolve_entry_point(const FunctionRoute& route, LbmEntryPoint& entry_point);
    static ipc::IpcResponseStatus to_ipc_response_status(LbmResponseCode code) noexcept;
    static ipc::IpcPayload error_payload(const std::string& message);

    LbmModuleLoaderOptions options_;
    FunclistRegistry registry_;
    bool configuration_loaded_{false};
    mutable std::mutex cache_mutex_;  // 防止同一 Worker 多线程同时初始化同一动态库。
    std::map<std::filesystem::path, CachedLibrary> libraries_;
};

}  // namespace oxmbs::lbm
