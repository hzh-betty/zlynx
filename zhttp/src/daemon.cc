#include "daemon.h"
#include "zhttp_logger.h"

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
#include <fstream>
#include <sstream>

namespace zhttp {

// 全局停止标志
static std::atomic<bool> g_stop_flag{false};

// 信号处理函数
static void signal_handler(int sig) {
  if (sig == SIGTERM || sig == SIGINT) {
    g_stop_flag.store(true, std::memory_order_release);
  }
}

// ========== ProcessInfo 实现 ==========

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

// ========== Daemon 实现 ==========

int Daemon::daemonize(const std::string &work_dir, bool close_std) {
  // 1. 创建子进程，父进程退出
  pid_t pid = fork();
  if (pid < 0) {
    ZHTTP_LOG_ERROR("Fork failed: {}", strerror(errno));
    return -1;
  }
  if (pid > 0) {
    // 父进程退出
    _exit(0);
  }

  // 2. 子进程成为会话首进程
  if (setsid() < 0) {
    ZHTTP_LOG_ERROR("Setsid failed: {}", strerror(errno));
    return -1;
  }

  // 3. 忽略SIGHUP信号
  signal(SIGHUP, SIG_IGN);

  // 4. 再次fork，确保进程不是会话首进程（防止获取控制终端）
  pid = fork();
  if (pid < 0) {
    ZHTTP_LOG_ERROR("Second fork failed: {}", strerror(errno));
    return -1;
  }
  if (pid > 0) {
    _exit(0);
  }

  // 5. 改变工作目录
  if (chdir(work_dir.c_str()) < 0) {
    ZHTTP_LOG_ERROR("Chdir to {} failed: {}", work_dir, strerror(errno));
    return -1;
  }

  // 6. 重设文件权限掩码
  umask(0);

  // 7. 关闭标准输入输出
  if (close_std) {
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    // 重定向到/dev/null
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
      dup2(fd, STDIN_FILENO);
      dup2(fd, STDOUT_FILENO);
      dup2(fd, STDERR_FILENO);
      if (fd > STDERR_FILENO) {
        close(fd);
      }
    }
  }

  ZHTTP_LOG_INFO("Daemon process started, PID: {}", getpid());
  return 0;
}

int Daemon::start_daemon(int argc, char **argv, MainCallback main_cb,
                         bool is_daemon, uint32_t restart_interval_sec) {
  if (!is_daemon) {
    return real_start(argc, argv, std::move(main_cb));
  }
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
  // 先转为守护进程（但不关闭标准输出，方便日志）
  if (daemon(1, 0) < 0) {
    ZHTTP_LOG_ERROR("daemon() failed: {}", strerror(errno));
    return -1;
  }

  ProcessInfo::instance().parent_id = getpid();
  ProcessInfo::instance().parent_start_time =
      static_cast<uint64_t>(std::time(nullptr));

  // 设置信号处理
  setup_signal_handlers();

  while (!g_stop_flag.load(std::memory_order_acquire)) {
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
      ZHTTP_LOG_ERROR("fork() failed: {} (errno={})", strerror(errno), errno);
      return -1;
    } else {
      // 父进程：等待子进程
      int status = 0;
      waitpid(pid, &status, 0);

      if (g_stop_flag.load(std::memory_order_acquire)) {
        ZHTTP_LOG_INFO("Daemon received stop signal, exiting...");
        break;
      }

      if (WIFEXITED(status)) {
        int exit_code = WEXITSTATUS(status);
        if (exit_code == 0) {
          ZHTTP_LOG_INFO("Worker process exited normally (PID: {})", pid);
          break; // 正常退出，不重启
        }
        ZHTTP_LOG_WARN("Worker process exited with code {} (PID: {})",
                       exit_code, pid);
      } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        ZHTTP_LOG_ERROR("Worker process killed by signal {} (PID: {})", sig,
                        pid);
      }

      // 重启
      ProcessInfo::instance().restart_count++;
      ZHTTP_LOG_INFO("Restarting worker process in {} seconds... (count: {})",
                     restart_interval_sec,
                     ProcessInfo::instance().restart_count);
      sleep(restart_interval_sec);
    }
  }

  return 0;
}

void Daemon::setup_signal_handlers() {
  struct sigaction sa {};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;

  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);

  // 忽略 SIGPIPE（防止写入关闭的socket导致进程终止）
  signal(SIGPIPE, SIG_IGN);
}

bool Daemon::should_stop() {
  return g_stop_flag.load(std::memory_order_acquire);
}

int Daemon::write_pid_file(const std::string &pid_file) {
  std::ofstream ofs(pid_file);
  if (!ofs) {
    ZHTTP_LOG_ERROR("Failed to open PID file: {}", pid_file);
    return -1;
  }

  ofs << getpid() << std::endl;
  if (!ofs) {
    ZHTTP_LOG_ERROR("Failed to write PID file: {}", pid_file);
    return -1;
  }

  ZHTTP_LOG_INFO("PID file written: {}", pid_file);
  return 0;
}

int Daemon::remove_pid_file(const std::string &pid_file) {
  if (unlink(pid_file.c_str()) < 0) {
    if (errno != ENOENT) {
      ZHTTP_LOG_ERROR("Failed to remove PID file {}: {}", pid_file,
                      strerror(errno));
      return -1;
    }
  }
  return 0;
}

int Daemon::read_pid_file(const std::string &pid_file) {
  std::ifstream ifs(pid_file);
  if (!ifs) {
    return -1;
  }

  int pid;
  ifs >> pid;
  if (!ifs) {
    return -1;
  }

  return pid;
}

bool Daemon::is_process_running(int pid) {
  if (pid <= 0) {
    return false;
  }

  // 发送信号0检查进程是否存在
  if (kill(pid, 0) == 0) {
    return true;
  }

  return errno != ESRCH;
}

bool Daemon::ensure_single_instance(const std::string &pid_file) {
  int old_pid = read_pid_file(pid_file);
  if (old_pid > 0 && is_process_running(old_pid)) {
    ZHTTP_LOG_ERROR("Another instance is already running with PID: {}",
                    old_pid);
    return false;
  }

  // 删除旧的PID文件
  remove_pid_file(pid_file);

  // 写入新的PID
  if (write_pid_file(pid_file) < 0) {
    return false;
  }

  return true;
}

} // namespace zhttp
