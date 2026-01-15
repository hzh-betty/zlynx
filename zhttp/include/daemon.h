#ifndef ZHTTP_DAEMON_H_
#define ZHTTP_DAEMON_H_

#include <cstdint>
#include <functional>
#include <string>
#include <sys/types.h>

namespace zhttp {

/**
 * @brief 进程信息结构体
 * 用于记录守护进程和工作进程的状态
 */
struct ProcessInfo {
  pid_t parent_id = 0;            // 父进程（守护进程）ID
  pid_t main_id = 0;              // 主进程（工作进程）ID
  uint64_t parent_start_time = 0; // 父进程启动时间
  uint64_t main_start_time = 0;   // 主进程启动时间
  uint32_t restart_count = 0;     // 重启次数

  /**
   * @brief 转换为字符串
   */
  std::string to_string() const;

  /**
   * @brief 获取全局单例
   */
  static ProcessInfo &instance();
};

/**
 * @brief 守护进程工具类
 * 提供将进程转为守护进程的功能
 */
class Daemon {
public:
  /**
   * @brief 主函数回调类型
   */
  using MainCallback = std::function<int(int argc, char **argv)>;

  /**
   * @brief 将当前进程转为守护进程
   * @param work_dir 工作目录，默认为根目录
   * @param close_std 是否关闭标准输入输出，默认true
   * @return 成功返回0，失败返回-1
   */
  static int daemonize(const std::string &work_dir = "/",
                       bool close_std = true);

  /**
   * @brief 启动守护进程（带子进程监控和自动重启）
   * @param argc 参数个数
   * @param argv 参数数组
   * @param main_cb 主函数回调
   * @param is_daemon 是否以守护进程模式运行
   * @param restart_interval_sec 子进程崩溃后重启间隔（秒）
   * @return 程序退出码
   */
  static int start_daemon(int argc, char **argv, MainCallback main_cb,
                          bool is_daemon = true,
                          uint32_t restart_interval_sec = 5);

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

  /**
   * @brief 设置信号处理器
   * 设置 SIGTERM, SIGINT 信号处理，用于优雅关闭
   */
  static void setup_signal_handlers();

  /**
   * @brief 检查是否收到停止信号
   */
  static bool should_stop();

private:
  /**
   * @brief 直接运行（非守护进程模式）
   */
  static int real_start(int argc, char **argv, MainCallback main_cb);

  /**
   * @brief 守护进程模式运行（带监控重启）
   */
  static int real_daemon(int argc, char **argv, MainCallback main_cb,
                         uint32_t restart_interval_sec);
};

} // namespace zhttp

#endif // ZHTTP_DAEMON_H_
