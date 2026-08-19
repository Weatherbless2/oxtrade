#include "oxapp/process/worker_supervisor.h"
#include "oxapp/process/worker_process.h"

#include <cassert>
#include <chrono>
#include <thread>

namespace {

int exit_with_error() {
    return 17;
}

void test_default_worker_pool() {
    oxapp::process::WorkerSupervisor supervisor(2);

    assert(supervisor.desired_count() == 2);
    assert(supervisor.start());
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

    assert(supervisor.stop(std::chrono::milliseconds{500}));
    assert(!supervisor.started());
    assert(supervisor.running_count() == 0);
    assert(supervisor.worker(0)->state() == oxapp::process::WorkerState::Stopped);
    assert(supervisor.worker(1)->state() == oxapp::process::WorkerState::Stopped);
}

void test_abnormal_exit_is_restarted() {
    oxapp::process::WorkerSupervisor supervisor(1, &exit_with_error);
    assert(supervisor.start());

    const auto initial_pid = supervisor.worker(0)->pid();
    bool restarted = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        supervisor.monitor_once();
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
    oxapp::process::WorkerProcess worker(0, &exit_with_error);
    assert(worker.start());

    bool exited = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (worker.poll_exit()) {
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
    oxapp::process::WorkerSupervisor supervisor(0);
    assert(!supervisor.start());
    assert(!supervisor.started());
}

}  // namespace

int main() {
    test_default_worker_pool();
    test_abnormal_exit_is_restarted();
    test_exit_status_is_recorded();
    test_zero_worker_count_is_rejected();
    return 0;
}
