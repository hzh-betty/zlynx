/**
 * daemon.cc
 * daemon 实现。
 *
 * @author hzh-betty
 */

#include "zhttp/runtime/daemon.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <array>
#include <stdexcept>
#include <fmt/format.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>

namespace zhttp {

// 全局停止标志
static_assert(std::atomic<bool>::is_always_lock_free, "Signal flag must be lock-free");
static std::atomic<bool> g_stop_flag{false};
static std::atomic_flag process_runner_active = ATOMIC_FLAG_INIT;

namespace {

int daemonize_process() {
    // 第一次 fork
    // 的目的不是拉起业务子进程，而是完成“守护化脱离终端”的基础步骤： 1)
    // 父进程立即退出，让调用方返回； 2) 子进程继续执行并通过 setsid()
    // 成为新的会话领导者，避免受原控制终端影响。 仅当当前进程还不是 init
    // 托管时进行 fork + setsid。
    if (getppid() != 1) {
        pid_t pid = fork();
        if (pid > 0) {
            _exit(0);
        }
        if (pid < 0) {
            fmt::print(stderr, "fork() failed while daemonizing: {} (errno={})\n",
                            strerror(errno), errno);
            return -1;
        }
        // 子进程继续执行，成为新的会话领导者和进程组领导者。
        if (setsid() < 0) {
            fmt::print(stderr, "setsid() failed: {} (errno={})\n", strerror(errno),
                            errno);
            return -1;
        }
    }

    // 与现有行为保持一致：保留工作目录和标准输出供日志系统使用。
    umask(0);
    return 0;
}

bool wait_for_child_exit(pid_t child_pid, int &status) {
    bool sent_sigterm = false;
    while (true) {
        if (g_stop_flag.load(std::memory_order_acquire) && !sent_sigterm) {
            kill(child_pid, SIGTERM);
            sent_sigterm = true;
        }
        pid_t wait_ret = waitpid(child_pid, &status, WNOHANG);
        if (wait_ret == 0) {
            ::usleep(10000);
            continue;
        }
        if (wait_ret == child_pid) {
            return true;
        }

        if (wait_ret < 0 && errno == EINTR) {
            if (g_stop_flag.load(std::memory_order_acquire) && !sent_sigterm) {
                kill(child_pid, SIGTERM);
                sent_sigterm = true;
            }
            continue;
        }

        fmt::print(stderr, "waitpid() failed: {} (errno={})\n", strerror(errno),
                        errno);
        return false;
    }
}

void sleep_before_restart(uint32_t restart_interval_sec) {
    unsigned int remaining = restart_interval_sec;
    while (remaining > 0 && !g_stop_flag.load(std::memory_order_acquire)) {
        remaining = sleep(remaining);
    }
}

// 这两个步骤仅服务 start_daemon；放在实现局部，不作为模块接口暴露。
int real_start(int argc, char **argv, Daemon::MainCallback main_cb) {
    ProcessInfo::instance().main_id = getpid();
    ProcessInfo::instance().main_start_time =
        static_cast<uint64_t>(std::time(nullptr));
    return main_cb(argc, argv);
}

int real_daemon(int argc, char **argv, Daemon::MainCallback main_cb,
                uint32_t restart_interval_sec) {
    // 先完成守护化（第一次 fork + setsid，或在 init/systemd 托管时跳过）。
    if (daemonize_process() < 0) {
        return -1;
    }

    ProcessInfo::instance().parent_id = getpid();
    ProcessInfo::instance().parent_start_time =
        static_cast<uint64_t>(std::time(nullptr));

    while (!g_stop_flag.load(std::memory_order_acquire)) {
        // 第二次 fork：创建真正执行业务逻辑的 worker。
        // 父进程只负责监督、等待和按策略重启；
        // 子进程只负责执行 main_cb。
        pid_t pid = fork();
        if (pid == 0) {
            // 子进程：执行用户主函数
            ProcessInfo::instance().main_id = getpid();
            ProcessInfo::instance().main_start_time =
                static_cast<uint64_t>(std::time(nullptr));
            fmt::print(stderr, "Worker process started, PID: {}\n", getpid());
            return real_start(argc, argv, main_cb);
        } else if (pid < 0) {
            // fork 失败
            fmt::print(stderr, "fork() failed: {} (errno={})\n", strerror(errno),
                            errno);
            return -1;
        } else {
            // 父进程：等待子进程
            int status = 0;
            if (!wait_for_child_exit(pid, status)) {
                return -1;
            }

            if (g_stop_flag.load(std::memory_order_acquire)) {
                fmt::print(stderr, "Daemon received stop signal, exiting...\n");
                break;
            }

            if (WIFEXITED(status)) {
                int exit_code = WEXITSTATUS(status);
                if (exit_code == 0) {
                    // 业务正常结束：守护进程也正常退出，不再拉起新 worker。
                    fmt::print(stderr, "Worker process exited normally (PID: {})\n",
                                   pid);
                    break; // 正常退出，不重启
                }
                // 业务异常退出：进入重启流程。
                fmt::print(stderr, "Worker process exited with code {} (PID: {})\n",
                               exit_code, pid);
            } else if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                // 被信号杀死：也进入重启流程（除非外部已触发全局停止标记）。
                fmt::print(stderr, "Worker process killed by signal {} (PID: {})\n",
                                sig, pid);
            }

            // 重启
            ProcessInfo::instance().restart_count++;
            fmt::print(stderr,
                "Restarting worker process in {} seconds... (count: {})\n",
                restart_interval_sec, ProcessInfo::instance().restart_count);
            sleep_before_restart(restart_interval_sec);
        }
    }

    return 0;
}

} // namespace

// 信号处理函数
static void signal_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        g_stop_flag.store(true, std::memory_order_release);
    }
}

ProcessInfo &ProcessInfo::instance() {
    static ProcessInfo s_instance;
    return s_instance;
}

std::string ProcessInfo::to_string() const {
    std::ostringstream ss;
    ss << "[ProcessInfo parent_id=" << parent_id << " main_id=" << main_id
       << " parent_start_time=" << parent_start_time
       << " main_start_time=" << main_start_time
       << " restart_count=" << restart_count << "]";
    return ss.str();
}

namespace {
class SignalScope {
  public:
    SignalScope() {
        struct sigaction action{};
        sigemptyset(&action.sa_mask);
        for (size_t i = 0; i < signals_.size(); ++i) {
            action.sa_handler = signals_[i] == SIGPIPE ? SIG_IGN : signal_handler;
            if (::sigaction(signals_[i], &action, &previous_[i]) < 0) {
                restore();
                throw std::runtime_error("Cannot install process signal handlers");
            }
            ++installed_;
        }
    }
    ~SignalScope() { restore(); }
  private:
    void restore() {
        while (installed_) {
            --installed_;
            ::sigaction(signals_[installed_], &previous_[installed_], nullptr);
        }
    }
    const std::array<int, 3> signals_{SIGTERM, SIGINT, SIGPIPE};
    std::array<struct sigaction, 3> previous_{};
    size_t installed_ = 0;
};
struct RunnerScope {
    ~RunnerScope() { process_runner_active.clear(std::memory_order_release); }
};
} // namespace

int Daemon::start_daemon(int argc, char **argv, MainCallback main_cb,
                         bool is_daemon, uint32_t restart_interval_sec) {
    if (!main_cb || process_runner_active.test_and_set(std::memory_order_acquire))
        return -1;
    RunnerScope runner_scope;
    g_stop_flag.store(false, std::memory_order_release);
    SignalScope signals;
    if (!is_daemon)
        return real_start(argc, argv, std::move(main_cb));
    // 监督进程只使用同步输出，工作进程在 fork 后构建 Runtime 和日志回调。
    return real_daemon(argc, argv, std::move(main_cb), restart_interval_sec);
}

bool Daemon::should_stop() {
    return g_stop_flag.load(std::memory_order_acquire);
}

} // namespace zhttp
