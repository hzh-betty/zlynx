#ifndef ZHTTP_DAEMON_H_
#define ZHTTP_DAEMON_H_

#include <cstdint>
#include <functional>
#include <string>
#include <sys/types.h>

namespace zhttp {

/**
 * @brief 进程信息结构体
 * @details
 * 守护进程模式下通常会存在父进程和真正执行业务的工作进程，
 * 该结构用于记录它们的 PID、启动时间和重启次数，便于排障和状态输出。
 */
struct ProcessInfo {
    pid_t parent_id = 0;            // 父进程（守护进程）ID
    pid_t main_id = 0;              // 主进程（工作进程）ID
    uint64_t parent_start_time = 0; // 父进程启动时间
    uint64_t main_start_time = 0;   // 主进程启动时间
    uint32_t restart_count = 0;     // 重启次数

    /**
     * @brief 转换为字符串
     * @return 便于日志输出的描述字符串
     */
    std::string to_string() const;

    /**
     * @brief 获取全局单例
     * @return 进程信息单例引用
     */
    static ProcessInfo &instance();
};

/**
 * @brief 守护进程工具类
 * @details
 * 该类封装了一组与服务进程部署相关的能力，例如：
 * 1. 前后台启动
 * 2. 子进程崩溃后的自动拉起
 * 3. 优雅退出信号处理
 */
class Daemon {
  public:
    /**
     * @brief 主函数回调类型
     * @details 回调的签名与普通 main 函数保持一致，便于复用现有启动逻辑。
     */
    using MainCallback = std::function<int(int argc, char **argv)>;

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
     * @brief 设置信号处理器
     * 设置 SIGTERM, SIGINT 信号处理，用于优雅关闭
     */
    static void setup_signal_handlers();

    /**
     * @brief 检查是否收到停止信号
     * @return true 表示进程应开始退出
     */
    static bool should_stop();

  private:
    /**
     * @brief 直接运行（非守护进程模式）
     * @return 主回调返回的退出码
     */
    static int real_start(int argc, char **argv, MainCallback main_cb);

    /**
     * @brief 守护进程模式运行（带监控重启）
     * @return 整个守护流程最终退出码
     */
    static int real_daemon(int argc, char **argv, MainCallback main_cb,
                           uint32_t restart_interval_sec);
};

} // namespace zhttp

#endif // ZHTTP_DAEMON_H_
