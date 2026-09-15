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
    std::size_t max_response_payload_bytes{
        ipc::TaskProtocolLimits{}.max_frame_bytes - ipc::IpcWireLayout::kHeaderBytes};  // 与运行时 MSGMAX 对齐，LBM 输出不会在 Response 编码阶段才溢出。
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

    FunclistLoadResult load_funclist(const std::filesystem::path& path);  // 启动阶段读取 XML 并原子更新路由；不因配置读取而 dlopen。
    LbmModuleResult execute(const ipc::IpcTaskMsg& task);  // 任务命中路由后懒加载库、调用函数，再复制出 Worker 自有结果。

    bool configuration_loaded() const noexcept;
    std::size_t configured_route_count() const noexcept;
    std::size_t loaded_library_count() const;
    std::size_t cached_function_count() const;

private:
    struct CachedLibrary {
        void* native_handle{nullptr};  // 仅 Loader 拥有，析构时 dlclose。
        std::map<std::string, LbmEntryPoint> functions;  // 一个库可缓存多个符号；函数指针有效期依赖 native_handle。
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
