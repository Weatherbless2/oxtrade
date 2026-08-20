#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace oxapp::process {

using ProcessId = std::int64_t;  // 统一保存进程 ID；用有符号类型是为了用 -1 表示“当前没有子进程”。
using WorkerEntry = int (*)();  // 子进程启动后执行的函数；返回值会被转换为子进程退出码。
using WorkerEntryWithContext = int (*)(void*);  // 需要运行时对象的 Worker 使用 C 风格上下文指针；父进程负责保证该对象存活到 Worker 停止。

enum class WorkerState {
    Created,    // 对象已创建，但还没有调用 start()。
    Starting,   // 已开始创建子进程，但启动流程尚未完成。
    Running,    // 子进程已创建，当前仍由本对象监管。
    Stopping,   // 已发送停止请求，正在等待子进程退出。
    Stopped,    // 子进程按预期退出，或本对象当前没有活动子进程。
    Crashed,    // 子进程异常退出，等待 supervisor 决定是否重启。
    Restarting, // 正在为异常退出的 Worker 创建新的子进程。
};

const char* worker_state_name(WorkerState state) noexcept;

class WorkerProcess {
public:
    explicit WorkerProcess(std::size_t worker_id, WorkerEntry entry = nullptr);
    WorkerProcess(std::size_t worker_id,
                  WorkerEntryWithContext entry,
                  void* entry_context);  // #7 Query Worker 通过受控上下文取得队列借用引用，不转移 IPC 所有权。
    ~WorkerProcess();  // 析构前主动停止子进程，避免对象销毁后留下孤儿进程。

    WorkerProcess(const WorkerProcess&) = delete;
    WorkerProcess& operator=(const WorkerProcess&) = delete;
    WorkerProcess(WorkerProcess&&) = delete;
    WorkerProcess& operator=(WorkerProcess&&) = delete;

    bool start();    // 首次启动 Worker；已经运行或正在停止时拒绝重复启动。
    bool poll_exit();  // 非阻塞检查子进程是否退出，并在退出时回收其状态。
    bool restart();  // 仅重启已退出的 Worker，并累计重启次数。
    bool stop(std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});  // 先优雅停止，超时再强制结束。

    std::size_t worker_id() const noexcept;  // 返回 supervisor 分配的稳定逻辑编号，而不是系统 PID。
    ProcessId pid() const noexcept;  // 返回当前子进程 PID；没有活动子进程时返回 -1。
    WorkerState state() const noexcept;  // 返回最近一次状态转换后的状态。
    std::size_t restart_count() const noexcept;  // 返回自动/手动重启成功尝试的累计次数。
    std::chrono::system_clock::time_point start_time() const noexcept;  // 返回当前这次启动的墙上时钟时间。
    bool has_exit_status() const noexcept;  // 表示是否已经成功回收过一次子进程退出状态。
    int last_exit_code() const noexcept;  // 正常退出时的退出码；被信号终止时为 -1。
    int last_exit_signal() const noexcept;  // 被信号终止时的信号编号；正常退出时为 0。

private:
    bool launch();  // 通过 fork 创建子进程，并在子进程中执行 WorkerEntry。
    void record_exit(int status) noexcept;  // 将 waitpid 返回的原始状态拆解为可查询的退出信息。

    std::size_t worker_id_;
    WorkerEntry entry_{nullptr};  // 业务层注入的无参入口；为空时使用默认的信号等待循环。
    WorkerEntryWithContext context_entry_{nullptr};  // 与 entry_ 二选一；存在时优先调用并传入 entry_context_。
    void* entry_context_{nullptr};  // 只借用父进程长期持有的运行时对象；Worker 不释放也不修改对象所有权。
    ProcessId pid_{-1};  // 当前被监管子进程的系统 PID。
    WorkerState state_{WorkerState::Created};  // 新建对象的初始生命周期状态。
    std::size_t restart_count_{0};  // 每次 restart() 进入新 launch() 前递增。
    std::chrono::system_clock::time_point start_time_{};  // 最近一次成功创建子进程的时间。
    bool has_exit_status_{false};  // 防止调用者把默认退出字段误认为真实回报。
    int last_exit_code_{-1};  // 默认无有效正常退出码。
    int last_exit_signal_{0};  // 默认没有记录到终止信号。
};

}  // namespace oxapp::process
