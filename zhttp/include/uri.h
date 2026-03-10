#ifndef ZHTTP_URI_H_
#define ZHTTP_URI_H_

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace zhttp {

/**
 * @brief URI 解析类
 *
 * 支持解析标准 URI 格式:
 *   scheme://[userinfo@]host[:port]/path[?query][#fragment]
 *
 * 示例:
 *   http://user:pass@example.com:8080/path?key=value#section
 */
class Uri {
public:
  using ptr = std::shared_ptr<Uri>;

  /**
   * @brief 创建 Uri 对象
   * @param uri URI 字符串
   * @return 解析成功返回 Uri 对象，否则返回 nullptr
   */
  static Uri::ptr create(const std::string &uri);

  /**
   * @brief 默认构造函数
   */
  Uri();

  // ========== Getter 方法 ==========

  /**
   * @brief 获取 URI scheme
   * @return 例如 http、https、ws
   */
  const std::string &scheme() const { return scheme_; }

  /**
   * @brief 获取 userinfo 部分
   * @return 例如 user:password
   */
  const std::string &userinfo() const { return userinfo_; }

  /**
   * @brief 获取主机名或 IP
   * @return host 部分
   */
  const std::string &host() const { return host_; }

  /**
   * @brief 获取路径部分
   * @return 若原始 URI 未显式写 path，通常会返回 "/"
   */
  const std::string &path() const;

  /**
   * @brief 获取 query 部分
   * @return 不含问号的原始 query 字符串
   */
  const std::string &query() const { return query_; }

  /**
   * @brief 获取 fragment 部分
   * @return 不含井号的片段字符串
   */
  const std::string &fragment() const { return fragment_; }

  /**
   * @brief 获取端口号
   * @return 显式端口；未显式指定时通常按 scheme 推导默认端口
   */
  int32_t port() const;

  // ========== Setter 方法 ==========

  /**
   * @brief 设置 scheme
   * @param v scheme 字符串
   */
  void set_scheme(const std::string &v) { scheme_ = v; }

  /**
   * @brief 设置 userinfo
   * @param v userinfo 字符串
   */
  void set_userinfo(const std::string &v) { userinfo_ = v; }

  /**
   * @brief 设置 host
   * @param v 主机名或 IP
   */
  void set_host(const std::string &v) { host_ = v; }

  /**
   * @brief 设置 path
   * @param v 路径字符串
   */
  void set_path(const std::string &v) { path_ = v; }

  /**
   * @brief 设置 query
   * @param v 查询字符串
   */
  void set_query(const std::string &v) { query_ = v; }

  /**
   * @brief 设置 fragment
   * @param v 片段字符串
   */
  void set_fragment(const std::string &v) { fragment_ = v; }

  /**
   * @brief 设置端口
   * @param v 端口号
   */
  void set_port(int32_t v) { port_ = v; }

  // ========== 工具方法 ==========

  /**
   * @brief 转换为字符串
    * @return 按 URI 语法重新组装后的完整字符串
   */
  std::string to_string() const;

  /**
   * @brief 获取 authority 部分 (userinfo@host:port)
    * @return authority 字符串
   */
  std::string authority() const;

  /**
   * @brief 检查是否使用默认端口
    * @return true 表示当前端口等于该 scheme 的默认端口
   */
  bool is_default_port() const;

  /**
   * @brief 获取 host:port 形式的字符串
    * @return host:port 组合字符串
   */
  std::string host_port() const;

  /**
   * @brief 解析 query 参数到 map
    * @return query 参数键值映射表
   */
  std::unordered_map<std::string, std::string> parse_query() const;

  /**
   * @brief 获取单个 query 参数
   * @param key 参数名
   * @param default_value 默认值
    * @return query 参数值
   */
  std::string query_param(const std::string &key,
                          const std::string &default_value = "") const;

private:
  /**
   * @brief 内部解析方法
   * @param uri 原始 URI 字符串
   * @return true 表示解析成功
   */
  bool parse(const std::string &uri);

  /**
   * @brief URL 解码
   * @param str 待解码字符串
   * @return 解码结果
   */
  static std::string url_decode(const std::string &str);

private:
  // URI 各组成部分。
  std::string scheme_;   // http, https, ws, wss 等
  std::string userinfo_; // user:password
  std::string host_;     // 主机名或 IP
  std::string path_;     // 路径
  std::string query_;    // 查询参数
  std::string fragment_; // 片段标识符
  int32_t port_ = 0;     // 端口号
};

} // namespace zhttp

#endif // ZHTTP_URI_H_
