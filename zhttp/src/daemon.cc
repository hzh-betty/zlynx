#include "zhttp/daemon.h"
#include "zhttp/zhttp_logger.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>

namespace zhttp {

// 全局停止标志
static std::atomic<bool> g_stop_flag{false};

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
            ZHTTP_LOG_ERROR("fork() failed while daemonizing: {} (errno={})",
                            strerror(errno), errno);
            return -1;
        }
        // 子进程继续执行，成为新的会话领导者和进程组领导者。
        if (setsid() < 0) {
            ZHTTP_LOG_ERROR("setsid() failed: {} (errno={})", strerror(errno),
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
        pid_t wait_ret = waitpid(child_pid, &status, 0);
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

        ZHTTP_LOG_ERROR("waitpid() failed: {} (errno={})", strerror(errno),
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

int Daemon::start_daemon(int argc, char **argv, MainCallback main_cb,
                         bool is_daemon, uint32_t restart_interval_sec) {
    g_stop_flag.store(false, std::memory_order_release);

    if (!main_cb) {
        ZHTTP_LOG_ERROR("main callback is empty");
        return -1;
    }

    // 统一安装信号处理，确保前台/后台模式都可优雅退出。
    setup_signal_handlers();

    if (!is_daemon) {
        // 前台模式：不进入守护父子模型，直接执行主逻辑。
        return real_start(argc, argv, std::move(main_cb));
    }
    // 后台模式：进入守护父进程 + worker 子进程模型。
    return real_daemon(argc, argv, std::move(main_cb), restart_interval_sec);
}

int Daemon::real_start(int argc, char **argv, MainCallback main_cb) {
    ProcessInfo::instance().main_id = getpid();
    ProcessInfo::instance().main_start_time =
        static_cast<uint64_t>(std::time(nullptr));
    return main_cb(argc, argv);
}

int Daemon::real_daemon(int argc, char **argv, MainCallback main_cb,
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
            ZHTTP_LOG_INFO("Worker process started, PID: {}", getpid());
            return real_start(argc, argv, main_cb);
        } else if (pid < 0) {
            // fork 失败
            ZHTTP_LOG_ERROR("fork() failed: {} (errno={})", strerror(errno),
                            errno);
            return -1;
        } else {
            // 父进程：等待子进程
            int status = 0;
            if (!wait_for_child_exit(pid, status)) {
                return -1;
            }

            if (g_stop_flag.load(std::memory_order_acquire)) {
                ZHTTP_LOG_INFO("Daemon received stop signal, exiting...");
                break;
            }

            if (WIFEXITED(status)) {
                int exit_code = WEXITSTATUS(status);
                if (exit_code == 0) {
                    // 业务正常结束：守护进程也正常退出，不再拉起新 worker。
                    ZHTTP_LOG_INFO("Worker process exited normally (PID: {})",
                                   pid);
                    break; // 正常退出，不重启
                }
                // 业务异常退出：进入重启流程。
                ZHTTP_LOG_WARN("Worker process exited with code {} (PID: {})",
                               exit_code, pid);
            } else if (WIFSIGNALED(status)) {
                int sig = WTERMSIG(status);
                // 被信号杀死：也进入重启流程（除非外部已触发全局停止标记）。
                ZHTTP_LOG_ERROR("Worker process killed by signal {} (PID: {})",
                                sig, pid);
            }

            // 重启
            ProcessInfo::instance().restart_count++;
            ZHTTP_LOG_INFO(
                "Restarting worker process in {} seconds... (count: {})",
                restart_interval_sec, ProcessInfo::instance().restart_count);
            sleep_before_restart(restart_interval_sec);
        }
    }

    return 0;
}

void Daemon::setup_signal_handlers() {
    struct sigaction sa {};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGTERM, &sa, nullptr) < 0) {
        ZHTTP_LOG_ERROR("sigaction(SIGTERM) failed: {} (errno={})",
                        strerror(errno), errno);
    }
    if (sigaction(SIGINT, &sa, nullptr) < 0) {
        ZHTTP_LOG_ERROR("sigaction(SIGINT) failed: {} (errno={})",
                        strerror(errno), errno);
    }

    // 忽略 SIGPIPE（防止写入关闭的socket导致进程终止）
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        ZHTTP_LOG_ERROR("signal(SIGPIPE) failed: {} (errno={})",
                        strerror(errno), errno);
    }
}

bool Daemon::should_stop() {
    return g_stop_flag.load(std::memory_order_acquire);
}

} // namespace zhttp
