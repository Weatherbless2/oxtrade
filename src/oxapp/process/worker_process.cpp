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

namespace oxapp::process {
namespace {

#if defined(__unix__) || defined(__APPLE__)
volatile std::sig_atomic_t stop_requested = 0;

void handle_stop_signal(int) noexcept {
    stop_requested = 1;
}

void install_worker_signal_handlers() noexcept {
    struct sigaction action {};
    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

int default_worker_entry() noexcept {
    stop_requested = 0;
    install_worker_signal_handlers();

    while (stop_requested == 0) {
        pause();
    }

    return 0;
}
#endif

}  // namespace

const char* worker_state_name(WorkerState state) noexcept {
    switch (state) {
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
    : worker_id_(worker_id), entry_(entry) {}

WorkerProcess::~WorkerProcess() {
    stop();
}

bool WorkerProcess::start() {
    if (pid_ > 0 || state_ == WorkerState::Running ||
        state_ == WorkerState::Starting || state_ == WorkerState::Stopping) {
        return false;
    }

    return launch();
}

bool WorkerProcess::launch() {
    state_ = WorkerState::Starting;

#if defined(__unix__) || defined(__APPLE__)
    const pid_t child_pid = ::fork();
    if (child_pid < 0) {
        state_ = WorkerState::Stopped;
        return false;
    }

    if (child_pid == 0) {
        const WorkerEntry worker_entry = entry_ != nullptr ? entry_ : default_worker_entry;
        const int exit_code = worker_entry();
        _exit(exit_code < 0 ? 1 : exit_code > 255 ? 255 : exit_code);
    }

    pid_ = static_cast<ProcessId>(child_pid);
    start_time_ = std::chrono::system_clock::now();
    state_ = WorkerState::Running;
    return true;
#else
    state_ = WorkerState::Stopped;
    return false;
#endif
}

bool WorkerProcess::poll_exit() {
#if defined(__unix__) || defined(__APPLE__)
    if (pid_ <= 0) {
        return false;
    }

    int status = 0;
    pid_t result = 0;
    do {
        result = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    } while (result < 0 && errno == EINTR);

    if (result == 0) {
        return false;
    }

    if (result == static_cast<pid_t>(pid_)) {
        record_exit(status);
        return true;
    }

    if (result < 0 && errno == ECHILD) {
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
    pid_ = -1;
    has_exit_status_ = true;
#if defined(__unix__) || defined(__APPLE__)
    last_exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    last_exit_signal_ = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
#else
    (void)status;
    last_exit_code_ = -1;
    last_exit_signal_ = 0;
#endif
    state_ = state_ == WorkerState::Stopping ? WorkerState::Stopped : WorkerState::Crashed;
}

bool WorkerProcess::restart() {
    if (pid_ > 0 || state_ == WorkerState::Running ||
        state_ == WorkerState::Starting || state_ == WorkerState::Stopping) {
        return false;
    }

    state_ = WorkerState::Restarting;
    ++restart_count_;
    if (launch()) {
        return true;
    }

    state_ = WorkerState::Crashed;
    return false;
}

bool WorkerProcess::stop(std::chrono::milliseconds timeout) {
#if defined(__unix__) || defined(__APPLE__)
    if (pid_ <= 0) {
        state_ = WorkerState::Stopped;
        return true;
    }

    state_ = WorkerState::Stopping;
    const pid_t child_pid = static_cast<pid_t>(pid_);
    if (::kill(child_pid, SIGTERM) < 0 && errno != ESRCH) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (pid_ > 0 && std::chrono::steady_clock::now() < deadline) {
        if (poll_exit()) {
            return state_ == WorkerState::Stopped;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    if (pid_ > 0) {
        ::kill(static_cast<pid_t>(pid_), SIGKILL);
        int status = 0;
        pid_t result = 0;
        do {
            result = ::waitpid(static_cast<pid_t>(pid_), &status, 0);
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
