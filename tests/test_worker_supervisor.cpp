#include "oxapp/process/worker_supervisor.h"
#include "oxapp/process/worker_process.h"

#include <cassert>
#include <chrono>
#include <thread>

namespace {

int exit_with_error() {
    return 17;  // 用固定非零退出码模拟 Worker 业务崩溃，便于验证重启和退出信息。
}

void test_default_worker_pool() {
    oxapp::process::WorkerSupervisor supervisor(2);  // 默认入口会阻塞等待停止信号。

    assert(supervisor.desired_count() == 2);
    assert(supervisor.start());  // 验证两个子进程都能成功创建。
    assert(supervisor.started());
    assert(supervisor.running_count() == 2);

    const auto* worker0 = supervisor.worker(0);
    const auto* worker1 = supervisor.worker(1);
    assert(worker0 != nullptr);
    assert(worker1 != nullptr);
    assert(worker0->pid() > 0);
    assert(worker1->pid() > 0);
    assert(worker0->pid() != worker1->pid());
    assert(worker0->state() == oxapp::process::WorkerState::Running);
    assert(worker1->state() == oxapp::process::WorkerState::Running);

    assert(supervisor.stop(std::chrono::milliseconds{500}));  // 验证 SIGTERM 优雅退出和 waitpid 回收。
    assert(!supervisor.started());
    assert(supervisor.running_count() == 0);
    assert(supervisor.worker(0)->state() == oxapp::process::WorkerState::Stopped);
    assert(supervisor.worker(1)->state() == oxapp::process::WorkerState::Stopped);
}

void test_abnormal_exit_is_restarted() {
    oxapp::process::WorkerSupervisor supervisor(1, &exit_with_error);  // 注入会立即退出的入口，制造异常退出事件。
    assert(supervisor.start());

    const auto initial_pid = supervisor.worker(0)->pid();
    bool restarted = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        supervisor.monitor_once();  // 非阻塞巡检，循环等待短命 Worker 被回收并重新拉起。
        const auto* worker = supervisor.worker(0);
        if (worker->restart_count() > 0 && worker->pid() > 0 && worker->pid() != initial_pid) {
            restarted = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    assert(restarted);
    assert(supervisor.running_count() == 1);
    assert(supervisor.worker(0)->restart_count() == 1);
    assert(supervisor.stop(std::chrono::milliseconds{500}));
}

void test_exit_status_is_recorded() {
    oxapp::process::WorkerProcess worker(0, &exit_with_error);  // 单独测试 Worker 的退出状态解析。
    assert(worker.start());

    bool exited = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (worker.poll_exit()) {  // poll_exit() 返回 true 表示已回收，而不是“仍在运行”。
            exited = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    assert(exited);
    assert(worker.state() == oxapp::process::WorkerState::Crashed);
    assert(worker.has_exit_status());
    assert(worker.last_exit_code() == 17);
    assert(worker.last_exit_signal() == 0);
    assert(worker.pid() == -1);

    assert(worker.restart());
    assert(worker.restart_count() == 1);
    assert(worker.stop(std::chrono::milliseconds{500}));
}

void test_zero_worker_count_is_rejected() {
    oxapp::process::WorkerSupervisor supervisor(0);  // 零数量配置没有可管理目标，必须被拒绝。
    assert(!supervisor.start());
    assert(!supervisor.started());
}

}  // namespace

int main() {
    test_default_worker_pool();  // 覆盖正常启动、状态查询和批量停止。
    test_abnormal_exit_is_restarted();  // 覆盖异常退出后的自动重启。
    test_exit_status_is_recorded();  // 覆盖退出码、信号和 PID 清理。
    test_zero_worker_count_is_rejected();  // 覆盖无效配置的输入校验。
    return 0;
}
