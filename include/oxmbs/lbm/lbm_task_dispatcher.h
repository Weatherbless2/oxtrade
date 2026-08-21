#pragma once

#include "oxmbs/lbm/lbm_module_loader.h"
#include "oxmbs/worker/query_worker_runtime.h"

#include <filesystem>

namespace oxmbs::lbm {

class LbmTaskDispatcher {
public:
    explicit LbmTaskDispatcher(LbmModuleLoaderOptions options = {});  // 构造 Worker 使用的分发门面，内部统一持有 Loader 生命周期。
    FunclistLoadResult load_funclist(const std::filesystem::path& path);  // 输入是 ini 解析出的 XML 路径；成功只建立白名单，不加载 .so。
    worker::QueryExecutionResult execute(const ipc::IpcTaskMsg& task);  // 输入是已过 deadline 的任务，输出统一 IPC 状态和自有 payload。
    static worker::QueryExecutionResult handle(const ipc::IpcTaskMsg& task,
                                               void* context);  // 适配 #7 Worker Handler，保持 IPC 和 deadline 边界不变。
    const LbmModuleLoader& loader() const noexcept;  // 只读暴露统计信息，调用方不能夺取句柄或修改缓存所有权。

private:
    LbmModuleLoader loader_;
};

}  // namespace oxmbs::lbm
