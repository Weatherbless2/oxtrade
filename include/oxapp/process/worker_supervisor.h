#pragma once

#include "oxapp/process/worker_process.h"

#include <chrono>
#include <cstddef>
#include <deque>

namespace oxapp::process {

class WorkerSupervisor {
public:
    explicit WorkerSupervisor(std::size_t desired_count = 2,
                              WorkerEntry entry = nullptr);  // 默认监管两个 Worker，符合工单二的初始拓扑。
    WorkerSupervisor(std::size_t desired_count,
                     WorkerEntryWithContext entry,
                     void* entry_context);  // #7 允许同质 Worker 共享一个只读运行时上下文，例如主进程创建的 IPC 队列句柄。
    ~WorkerSupervisor();  // 离开作用域时统一回收所有仍在运行的 Worker。

    WorkerSupervisor(const WorkerSupervisor&) = delete;
    WorkerSupervisor& operator=(const WorkerSupervisor&) = delete;

    bool start();  // 创建 Worker 对象并启动目标数量的子进程。
    void monitor_once();  // 执行一次非阻塞巡检；主循环可按固定周期反复调用。
    bool stop(std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});  // 请求所有 Worker 停止并等待回收。

    std::size_t desired_count() const noexcept;  // 返回配置的目标 Worker 数量。
    std::size_t running_count() const noexcept;  // 统计当前处于 Running 且 PID 有效的 Worker。
    bool started() const noexcept;  // 表示 supervisor 是否已成功完成 start()。
    const WorkerProcess* worker(std::size_t worker_id) const noexcept;  // 按逻辑编号只读查询 Worker，不转移所有权。

private:
    std::size_t desired_count_;  // 本 supervisor 需要维持的 Worker 数量。
    WorkerEntry entry_{nullptr};  // 传给每个 Worker 的统一无参入口函数。
    WorkerEntryWithContext context_entry_{nullptr};  // 带上下文入口；与 entry_ 二选一，便于兼容工单二的无参测试入口。
    void* entry_context_{nullptr};  // 借用对象必须在 stop() 回收所有 Worker 后才可以销毁。
    std::deque<WorkerProcess> workers_;  // 保存 Worker 对象，使其生命周期覆盖整个监管过程。
    bool started_{false};  // start() 成功后为 true，stop() 完成后恢复为 false。
    bool stopping_{false};  // stop() 期间阻止 monitor_once() 自动重启正在退出的 Worker。
};

}  // namespace oxapp::process
