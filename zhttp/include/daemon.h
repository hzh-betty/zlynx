#ifndef ZHTTP_DAEMON_H_
#define ZHTTP_DAEMON_H_

#include <string>

namespace zhttp {

/**
 * @brief 守护进程工具类
 * 提供将进程转为守护进程的功能
 */
class Daemon {
public:
  /**
   * @brief 将当前进程转为守护进程
   * @param work_dir 工作目录，默认为根目录
   * @param close_std 是否关闭标准输入输出，默认true
   * @return 成功返回0，失败返回-1
   */
  static int daemonize(const std::string &work_dir = "/",
                       bool close_std = true);

  /**
   * @brief 写入PID文件
   * @param pid_file PID文件路径
   * @return 成功返回0，失败返回-1
   */
  static int write_pid_file(const std::string &pid_file);

  /**
   * @brief 删除PID文件
   * @param pid_file PID文件路径
   * @return 成功返回0，失败返回-1
   */
  static int remove_pid_file(const std::string &pid_file);

  /**
   * @brief 从PID文件读取PID
   * @param pid_file PID文件路径
   * @return 成功返回PID，失败返回-1
   */
  static int read_pid_file(const std::string &pid_file);

  /**
   * @brief 检查进程是否运行
   * @param pid 进程ID
   * @return 运行返回true，否则返回false
   */
  static bool is_process_running(int pid);

  /**
   * @brief 单实例检查（通过PID文件）
   * @param pid_file PID文件路径
   * @return 如果已有实例运行返回false，否则返回true
   */
  static bool ensure_single_instance(const std::string &pid_file);
};

} // namespace zhttp

#endif // ZHTTP_DAEMON_H_
