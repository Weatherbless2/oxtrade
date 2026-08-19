#include "oxapp/process/worker_process.h"

#include <cerrno>
#include <chrono>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
// 封装单个 Worker 的启动、停止、状态、PID、退出码和重启次数。
namespace oxapp::process {
namespace {

#if defined(__unix__) || defined(__APPLE__)
volatile std::sig_atomic_t stop_requested = 0;  // 信号处理器与默认 Worker 之间共享的轻量停止标志。

void handle_stop_signal(int) noexcept {
    stop_requested = 1;  // 信号处理器只做异步信号安全的赋值，不在信号上下文中执行复杂逻辑。
}

void install_worker_signal_handlers() noexcept {
    struct sigaction action {};  // 为子进程准备统一的 SIGINT/SIGTERM 处理配置。
    action.sa_handler = handle_stop_signal;  // 收到停止信号时只设置标志，让主循环自行收尾。
    sigemptyset(&action.sa_mask);  // 处理当前信号时不额外屏蔽其他信号。
    action.sa_flags = 0;  // 保持最小信号处理配置；pause() 收到信号后会返回，让循环检查停止标志。
    sigaction(SIGINT, &action, nullptr);  // 支持终端 Ctrl-C 的停止方式。
    sigaction(SIGTERM, &action, nullptr);  // 支持 supervisor 的优雅停止方式。
}

int default_worker_entry() noexcept {
    stop_requested = 0;  // fork 后重新初始化，避免继承父进程此前的停止状态。
    install_worker_signal_handlers();  // 默认 Worker 暂时没有业务循环，只等待停止信号。

    while (stop_requested == 0) {  // 工单二只验证进程生命周期，后续业务循环会替换这里。
        pause();  // 让出 CPU，直到收到信号；不会忙等消耗主机资源。
    }

    return 0;  // 收到 SIGINT/SIGTERM 后用正常退出码结束默认 Worker。
}
#endif

}  // namespace

const char* worker_state_name(WorkerState state) noexcept {
    switch (state) {  // 将内部枚举转换为日志/调试可读的稳定字符串。
    case WorkerState::Created:
        return "Created";
    case WorkerState::Starting:
        return "Starting";
    case WorkerState::Running:
        return "Running";
    case WorkerState::Stopping:
        return "Stopping";
    case WorkerState::Stopped:
        return "Stopped";
    case WorkerState::Crashed:
        return "Crashed";
    case WorkerState::Restarting:
        return "Restarting";
    }

    return "Unknown";
}

WorkerProcess::WorkerProcess(std::size_t worker_id, WorkerEntry entry)
    : worker_id_(worker_id), entry_(entry) {}  // 只保存逻辑编号和入口，真正的 PID 在 start() 时产生。

WorkerProcess::~WorkerProcess() {
    stop();
}

bool WorkerProcess::start() {
    // 防止重复 fork：已有活动 PID 或处于生命周期过渡态时，调用者必须先 stop()。
    if (pid_ > 0 || state_ == WorkerState::Running ||
        state_ == WorkerState::Starting || state_ == WorkerState::Stopping) {
        return false;
    }

    return launch();
}

bool WorkerProcess::launch() {
    state_ = WorkerState::Starting;  // 先标记过渡态，失败时再回落到 Stopped。

#if defined(__unix__) || defined(__APPLE__)
    const pid_t child_pid = ::fork();  // POSIX 下复制当前进程，返回值区分父进程和子进程。
    if (child_pid < 0) {
        state_ = WorkerState::Stopped;  // fork 失败没有可回收的子进程。
        return false;
    }

    if (child_pid == 0) {
        const WorkerEntry worker_entry = entry_ != nullptr ? entry_ : default_worker_entry;  // 空入口使用占位 Worker。
        const int exit_code = worker_entry();  // 子进程执行入口，不应回到父进程的管理逻辑。
        _exit(exit_code < 0 ? 1 : exit_code > 255 ? 255 : exit_code);  // 退出码限制在 POSIX 的 0~255 范围。
    }

    pid_ = static_cast<ProcessId>(child_pid);  // 父进程保存子进程 PID，后续 kill/waitpid 都依赖它。
    start_time_ = std::chrono::system_clock::now();  // 记录本轮 Worker 的启动时刻。
    state_ = WorkerState::Running;  // fork 成功后认为 Worker 已进入可监管状态。
    return true;
#else
    state_ = WorkerState::Stopped;
    return false;
#endif
}

bool WorkerProcess::poll_exit() {
#if defined(__unix__) || defined(__APPLE__)
    if (pid_ <= 0) {  // 没有活动子进程时无需调用 waitpid。
        return false;
    }

    int status = 0;
    pid_t result = 0;
    do {
        result = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);  // 非阻塞查询，避免监控循环卡住。
    } while (result < 0 && errno == EINTR);

    if (result == 0) {  // 子进程仍在运行，本次巡检不改变状态。
        return false;
    }

    if (result == static_cast<pid_t>(pid_)) {
        record_exit(status);  // waitpid 已回收僵尸进程，同时解析退出原因。
        return true;
    }

    if (result < 0 && errno == ECHILD) {  // 子进程已被其他代码回收，当前对象无法取得真实退出码。
        pid_ = -1;
        state_ = WorkerState::Crashed;
        has_exit_status_ = false;
        return true;
    }
#else
    (void)this;
#endif

    return false;
}

void WorkerProcess::record_exit(int status) noexcept {
    pid_ = -1;  // 回收完成后清除旧 PID，防止后续误向已结束进程发送信号。
    has_exit_status_ = true;  // 下面的退出字段现在才具有真实含义。
#if defined(__unix__) || defined(__APPLE__)
    last_exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;  // 正常退出提取 exit() 的值。
    last_exit_signal_ = WIFSIGNALED(status) ? WTERMSIG(status) : 0;  // 信号终止时记录信号编号。
#else
    (void)status;
    last_exit_code_ = -1;
    last_exit_signal_ = 0;
#endif
    state_ = state_ == WorkerState::Stopping ? WorkerState::Stopped : WorkerState::Crashed;  // 区分主动停止与异常退出。
}

bool WorkerProcess::restart() {
    // 只有已经退出的 Worker 才允许重启；运行中的 Worker 不会被隐式替换。
    if (pid_ > 0 || state_ == WorkerState::Running ||
        state_ == WorkerState::Starting || state_ == WorkerState::Stopping) {
        return false;
    }

    state_ = WorkerState::Restarting;  // 先记录重启阶段，便于监控和日志观察。
    ++restart_count_;  // 统计本次重启尝试；即使 launch 失败也保留尝试次数。
    if (launch()) {
        return true;
    }

    state_ = WorkerState::Crashed;  // 新进程创建失败，保留异常状态等待上层处理。
    return false;
}

bool WorkerProcess::stop(std::chrono::milliseconds timeout) {
#if defined(__unix__) || defined(__APPLE__)
    if (pid_ <= 0) {  // 已退出或从未启动的 Worker 视为停止完成。
        state_ = WorkerState::Stopped;
        return true;
    }

    state_ = WorkerState::Stopping;  // 先进入停止态，record_exit() 才能判定为正常停止。
    const pid_t child_pid = static_cast<pid_t>(pid_);
    if (::kill(child_pid, SIGTERM) < 0 && errno != ESRCH) {  // ESRCH 表示进程已不存在，其他错误需要报告失败。
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;  // 以单调时钟计算超时，避免系统时间回拨影响等待。
    while (pid_ > 0 && std::chrono::steady_clock::now() < deadline) {
        if (poll_exit()) {  // 轮询期间一旦回收完成，立即返回最终停止结果。
            return state_ == WorkerState::Stopped;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});  // 降低轮询频率，避免忙等。
    }

    if (pid_ > 0) {
        ::kill(static_cast<pid_t>(pid_), SIGKILL);  // 优雅停止超时后使用强制信号兜底。
        int status = 0;
        pid_t result = 0;
        do {
            result = ::waitpid(static_cast<pid_t>(pid_), &status, 0);  // 阻塞回收，确保不遗留僵尸进程。
        } while (result < 0 && errno == EINTR);

        if (result == static_cast<pid_t>(pid_)) {
            record_exit(status);
            return state_ == WorkerState::Stopped;
        }

        return false;
    }

    return state_ == WorkerState::Stopped;
#else
    (void)timeout;
    state_ = WorkerState::Stopped;
    return false;
#endif
}

std::size_t WorkerProcess::worker_id() const noexcept {
    return worker_id_;
}

ProcessId WorkerProcess::pid() const noexcept {
    return pid_;
}

WorkerState WorkerProcess::state() const noexcept {
    return state_;
}

std::size_t WorkerProcess::restart_count() const noexcept {
    return restart_count_;
}

std::chrono::system_clock::time_point WorkerProcess::start_time() const noexcept {
    return start_time_;
}

bool WorkerProcess::has_exit_status() const noexcept {
    return has_exit_status_;
}

int WorkerProcess::last_exit_code() const noexcept {
    return last_exit_code_;
}

int WorkerProcess::last_exit_signal() const noexcept {
    return last_exit_signal_;
}

}  // namespace oxapp::process
