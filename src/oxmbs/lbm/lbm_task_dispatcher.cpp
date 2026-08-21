#include "oxmbs/lbm/lbm_task_dispatcher.h"

#include <string>

namespace oxmbs::lbm {

LbmTaskDispatcher::LbmTaskDispatcher(LbmModuleLoaderOptions options)
    : loader_(options) {
}

FunclistLoadResult LbmTaskDispatcher::load_funclist(const std::filesystem::path& path) {
    return loader_.load_funclist(path);
}

worker::QueryExecutionResult LbmTaskDispatcher::execute(const ipc::IpcTaskMsg& task) {
    const auto module_result = loader_.execute(task);
    worker::QueryExecutionResult result;
    result.status = module_result.response_status;
    result.payload = module_result.payload;
    return result;  // Worker 只接收统一 Response，不向 IPC 泄漏 dlopen/dlsym 细节。
}

worker::QueryExecutionResult LbmTaskDispatcher::handle(const ipc::IpcTaskMsg& task, void* context) {
    if (context == nullptr) {
        worker::QueryExecutionResult result;
        result.status = ipc::IpcResponseStatus::Failed;
        const std::string error = "LBM dispatcher context is null";
        result.payload.assign(error.begin(), error.end());
        return result;
    }
    return static_cast<LbmTaskDispatcher*>(context)->execute(task);
}

const LbmModuleLoader& LbmTaskDispatcher::loader() const noexcept {
    return loader_;
}

}  // namespace oxmbs::lbm
