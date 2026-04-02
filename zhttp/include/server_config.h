#ifndef ZHTTP_SERVER_CONFIG_H_
#define ZHTTP_SERVER_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace zhttp {

/**
 * @brief 协程栈模式
 * @details
 * 独立栈模式更直观；共享栈模式通常更省内存，但对运行时切换策略有额外要求。
 */
enum class StackMode {
  INDEPENDENT, // 独立栈模式（默认）
  SHARED       // 共享栈模式
};

struct ModuleLogConfig {
  std::string level;
  std::string format;
  std::string sink;
  std::string file;
};

/**
 * @brief HTTP 服务器配置
 * @details
 * 该结构汇总了服务器运行时常见的所有可调参数，主要作为 Builder 和
 * 配置加载流程之间的内部承载对象。
 */
struct ServerConfig {
  // 网络配置。
  std::string host = "0.0.0.0";
  uint16_t port = 8080;

  // 线程与协程配置。
  size_t num_threads = 4;
  StackMode stack_mode = StackMode::INDEPENDENT;

  // SSL/TLS 配置。
  bool enable_https = false;
  std::string cert_file;
  std::string key_file;
  bool force_http_to_https = false;
  uint16_t redirect_http_port = 80;

  // 服务器行为配置。
  std::string server_name = "zhttp/1.0";
  std::string homepage;
  bool daemon = false;

  // 日志配置。
  std::string log_level = "info";
  std::string log_format = "[%d{%Y-%m-%d %H:%M:%S}][%c][%p][%t] %m%n";
  std::string log_sink = "stdout";
  std::string log_file;
  ModuleLogConfig zcoroutine_log;
  ModuleLogConfig znet_log;
  ModuleLogConfig zhttp_log;

  // 超时配置，单位毫秒。
  uint64_t read_timeout = 30000;
  uint64_t write_timeout = 30000;
  uint64_t keepalive_timeout = 60000;

  // 限流配置。
  bool rate_limit_enabled = false;
  std::string rate_limit_type = "token_bucket"; // fixed_window/sliding_window/token_bucket
  size_t rate_limit_capacity = 10;              // capacity per time unit
  std::string rate_limit_time_unit = "second"; // millisecond/second/minute/hour

  /**
   * @brief 从 TOML 文件加载配置
   * @param filepath TOML 配置文件路径
   * @return 加载的配置
   * @throws std::runtime_error 如果文件不存在或解析失败
   */
  static ServerConfig from_toml(const std::string &filepath);

  /**
   * @brief 验证配置有效性
   * @return true 表示配置组合可用于启动服务
   */
  bool validate() const;
};

/**
 * @brief 将字符串转换为 StackMode
 * @param str 字符串形式的栈模式
 * @return 对应的枚举值
 */
StackMode string_to_stack_mode(const std::string &str);

/**
 * @brief 将 StackMode 转换为字符串
 * @param mode 栈模式枚举
 * @return 字符串形式的模式名
 */
std::string stack_mode_to_string(StackMode mode);

} // namespace zhttp

#endif // ZHTTP_SERVER_CONFIG_H_
