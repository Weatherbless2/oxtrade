#include "oxmbs/lbm/lbm_task_dispatcher.h"

#include <string>

namespace oxmbs::lbm {

LbmTaskDispatcher::LbmTaskDispatcher(LbmModuleLoaderOptions options)
    : loader_(options) {  // Dispatcher 只拥有一个 Loader，把路由、缓存和 ABI 细节集中到单一生命周期内。
}

FunclistLoadResult LbmTaskDispatcher::load_funclist(const std::filesystem::path& path) {
    return loader_.load_funclist(path);  // 启动期只校验路由，不提前加载所有业务 .so。
}

bool LbmTaskDispatcher::initialize_worker(std::string& error) noexcept {
    if (!loader_.configuration_loaded()) {
        error = "LBM funclist must be loaded before worker initialization";
        return false;
    }
    error.clear();
    return true;  // 动态库仍保持懒加载；每个 fork 后 Worker 只初始化自己的 Loader 运行边界。
}

void LbmTaskDispatcher::register_builtin_route(std::uint32_t msg_type,
                                               std::uint32_t func_id,
                                               BuiltinHandler handler,
                                               void* context) {
    if (msg_type == 0 || func_id == 0 || handler == nullptr) {
        return;  // 非法登记直接忽略，避免出现空函数指针或可匹配所有任务的隐式路由。
    }
    builtin_routes_[FunctionRouteKey{msg_type, func_id}] = BuiltinRoute{handler, context};
}

worker::QueryExecutionResult LbmTaskDispatcher::execute(const ipc::IpcTaskMsg& task) {
    const auto module_result = loader_.execute(task);  // XML 正式路由优先，命中后只经过 dlopen/dlsym 和 C ABI。
    if (module_result.module_status != LbmModuleStatus::RouteNotFound) {
        worker::QueryExecutionResult result;  // 把正式模块结果转换为 Worker 统一响应，隐藏 Loader 私有状态。
        result.status = module_result.response_status;
        result.payload = module_result.payload;
        return result;
    }

    const auto builtin = builtin_routes_.find(FunctionRouteKey{task.msg_type, task.func_id});
    if (builtin == builtin_routes_.end()) {
        worker::QueryExecutionResult result;
        result.status = module_result.response_status;
        result.payload = module_result.payload;
        return result;  // 未登记的编号仍按 RouteNotFound 拒绝，不能扩大成任意任务回退。
    }

    const auto execution = builtin->second.handler(task, builtin->second.context);  // 迁移期仅桥接已存在 Service，正式 XML 路由不会走这里。
    worker::QueryExecutionResult result;  // 使用 Worker 认识的统一结果类型隔离动态库实现细节。
    result.status = execution.status;
    result.payload = execution.payload;  // 复制桥接结果，Dispatcher 不把业务对象所有权泄露给 IPC。
    return result;  // Worker 只接收统一 Response，不向 IPC 泄漏 dlopen/dlsym 细节。
}

worker::QueryExecutionResult LbmTaskDispatcher::handle(const ipc::IpcTaskMsg& task, void* context) {
    if (context == nullptr) {  // Supervisor/Worker 配置错误时立即生成 Failed，不解引用空指针崩溃。
        worker::QueryExecutionResult result;
        result.status = ipc::IpcResponseStatus::Failed;
        const std::string error = "LBM dispatcher context is null";
        result.payload.assign(error.begin(), error.end());
        return result;
    }
    return static_cast<LbmTaskDispatcher*>(context)->execute(task);  // 由 QueryWorkerRuntime 的函数指针入口转回真实 Dispatcher。
}

const LbmModuleLoader& LbmTaskDispatcher::loader() const noexcept {
    return loader_;
}

std::size_t LbmTaskDispatcher::builtin_route_count() const noexcept {
    return builtin_routes_.size();
}

}  // namespace oxmbs::lbm
