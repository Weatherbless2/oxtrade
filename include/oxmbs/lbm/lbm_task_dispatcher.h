#pragma once

#include "oxmbs/lbm/lbm_module_loader.h"
#include "oxmbs/worker/query_worker_runtime.h"

#include <filesystem>
#include <map>

namespace oxmbs::lbm {

class LbmTaskDispatcher {
public:
    using BuiltinHandler = worker::QueryTaskHandler;  // 迁移期桥接仍复用 Worker 的统一入口，不改变 IPC/Response 所有权。

    explicit LbmTaskDispatcher(LbmModuleLoaderOptions options = {});  // 构造 Worker 使用的分发门面，内部统一持有 Loader 生命周期。
    FunclistLoadResult load_funclist(const std::filesystem::path& path);  // 输入是 ini 解析出的 XML 路径；成功只建立白名单，不加载 .so。
    bool initialize_worker(std::string& error) noexcept;  // fork 后由每个 Worker 调用，确认本地 Loader 已完成启动期配置。
    void register_builtin_route(std::uint32_t msg_type,
                                std::uint32_t func_id,
                                BuiltinHandler handler,
                                void* context);  // 仅登记尚未迁移到正式 .so 的明确路由，不能作为通配回退。
    worker::QueryExecutionResult execute(const ipc::IpcTaskMsg& task);  // 输入是已过 deadline 的任务，输出统一 IPC 状态和自有 payload。
    static worker::QueryExecutionResult handle(const ipc::IpcTaskMsg& task,
                                               void* context);  // 适配 #7 Worker Handler，保持 IPC 和 deadline 边界不变。
    const LbmModuleLoader& loader() const noexcept;  // 只读暴露统计信息，调用方不能夺取句柄或修改缓存所有权。
    std::size_t builtin_route_count() const noexcept;

private:
    struct BuiltinRoute {
        BuiltinHandler handler{nullptr};
        void* context{nullptr};  // 只借用主进程配置对象的 fork 副本，不由 Dispatcher 释放。
    };

    LbmModuleLoader loader_;
    std::map<FunctionRouteKey, BuiltinRoute> builtin_routes_;
};

}  // namespace oxmbs::lbm
