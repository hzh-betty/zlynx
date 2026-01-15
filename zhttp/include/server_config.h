#ifndef ZHTTP_SERVER_CONFIG_H_
#define ZHTTP_SERVER_CONFIG_H_

#include <cstdint>
#include <string>
#include <vector>

namespace zhttp {

/**
 * @brief 协程栈模式
 */
enum class StackMode {
  INDEPENDENT, // 独立栈模式（默认）
  SHARED       // 共享栈模式
};

/**
 * @brief HTTP 服务器配置
 */
struct ServerConfig {
  // 网络配置
  std::string host = "0.0.0.0";
  uint16_t port = 8080;

  // 线程配置
  size_t num_threads = 4;
  StackMode stack_mode = StackMode::INDEPENDENT;

  // SSL/TLS 配置
  bool enable_https = false;
  std::string cert_file;
  std::string key_file;

  // 服务器配置
  std::string server_name = "zhttp/1.0";
  bool daemon = false;

  // 日志配置
  std::string log_level = "info";
  std::string log_file;

  // 超时配置 (毫秒)
  uint64_t read_timeout = 30000;
  uint64_t write_timeout = 30000;
  uint64_t keepalive_timeout = 60000;

  // 缓冲区配置 (字节)
  size_t max_body_size = 10 * 1024 * 1024; // 10MB
  size_t buffer_size = 8192;

  /**
   * @brief 从 TOML 文件加载配置
   * @param filepath TOML 配置文件路径
   * @return 加载的配置
   * @throws std::runtime_error 如果文件不存在或解析失败
   */
  static ServerConfig from_toml(const std::string &filepath);

  /**
   * @brief 从 TOML 字符串加载配置
   * @param toml_content TOML 内容字符串
   * @return 加载的配置
   */
  static ServerConfig from_toml_string(const std::string &toml_content);

  /**
   * @brief 验证配置有效性
   * @return 是否有效
   */
  bool validate() const;

  /**
   * @brief 导出为 TOML 字符串
   */
  std::string to_toml_string() const;
};

/**
 * @brief 将字符串转换为 StackMode
 */
StackMode string_to_stack_mode(const std::string &str);

/**
 * @brief 将 StackMode 转换为字符串
 */
std::string stack_mode_to_string(StackMode mode);

} // namespace zhttp

#endif // ZHTTP_SERVER_CONFIG_H_
