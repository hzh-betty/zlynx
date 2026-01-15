#include "daemon.h"
#include "zhttp_logger.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace zhttp {

int Daemon::daemonize(const std::string &work_dir, bool close_std) {
  // 1. 创建子进程，父进程退出
  pid_t pid = fork();
  if (pid < 0) {
    ZHTTP_LOG_ERROR("Fork failed: {}", strerror(errno));
    return -1;
  }
  if (pid > 0) {
    // 父进程退出
    exit(0);
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
    exit(0);
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
