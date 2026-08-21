#include "oxmbs/lbm/lbm_task_dispatcher.h"

#include <string>

namespace oxmbs::lbm {

LbmTaskDispatcher::LbmTaskDispatcher(LbmModuleLoaderOptions options)
    : loader_(options) {  // Dispatcher 只拥有一个 Loader，把路由、缓存和 ABI 细节集中到单一生命周期内。
}

FunclistLoadResult LbmTaskDispatcher::load_funclist(const std::filesystem::path& path) {
    return loader_.load_funclist(path);  // 启动期只校验路由，不提前加载所有业务 .so。
}

worker::QueryExecutionResult LbmTaskDispatcher::execute(const ipc::IpcTaskMsg& task) {
    const auto module_result = loader_.execute(task);  // 把一个已过 deadline 校验的 Worker 任务交给 LBM 层处理。
    worker::QueryExecutionResult result;  // 使用 Worker 认识的统一结果类型隔离动态库实现细节。
    result.status = module_result.response_status;  // 业务拒绝/失败状态直接沿 IPC 应答语义返回。
    result.payload = module_result.payload;  // 复制结果字节，Dispatcher 不把 Loader 的内部缓存暴露出去。
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

}  // namespace oxmbs::lbm
