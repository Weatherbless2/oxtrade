#pragma once

#include "oxmbs/lbm/lbm_module_loader.h"
#include "oxmbs/worker/query_worker_runtime.h"

#include <filesystem>

namespace oxmbs::lbm {

class LbmTaskDispatcher {
public:
    explicit LbmTaskDispatcher(LbmModuleLoaderOptions options = {});
    FunclistLoadResult load_funclist(const std::filesystem::path& path);  // 启动期校验 XML，首次任务才加载 .so。
    worker::QueryExecutionResult execute(const ipc::IpcTaskMsg& task);
    static worker::QueryExecutionResult handle(const ipc::IpcTaskMsg& task,
                                               void* context);  // 适配 #7 Worker Handler，保持 IPC 和 deadline 边界不变。
    const LbmModuleLoader& loader() const noexcept;

private:
    LbmModuleLoader loader_;
};

}  // namespace oxmbs::lbm
