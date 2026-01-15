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

  const std::string &scheme() const { return scheme_; }
  const std::string &userinfo() const { return userinfo_; }
  const std::string &host() const { return host_; }
  const std::string &path() const;
  const std::string &query() const { return query_; }
  const std::string &fragment() const { return fragment_; }
  int32_t port() const;

  // ========== Setter 方法 ==========

  void set_scheme(const std::string &v) { scheme_ = v; }
  void set_userinfo(const std::string &v) { userinfo_ = v; }
  void set_host(const std::string &v) { host_ = v; }
  void set_path(const std::string &v) { path_ = v; }
  void set_query(const std::string &v) { query_ = v; }
  void set_fragment(const std::string &v) { fragment_ = v; }
  void set_port(int32_t v) { port_ = v; }

  // ========== 工具方法 ==========

  /**
   * @brief 转换为字符串
   */
  std::string to_string() const;

  /**
   * @brief 获取 authority 部分 (userinfo@host:port)
   */
  std::string authority() const;

  /**
   * @brief 检查是否使用默认端口
   */
  bool is_default_port() const;

  /**
   * @brief 获取 host:port 形式的字符串
   */
  std::string host_port() const;

  /**
   * @brief 解析 query 参数到 map
   */
  std::unordered_map<std::string, std::string> parse_query() const;

  /**
   * @brief 获取单个 query 参数
   * @param key 参数名
   * @param default_value 默认值
   */
  std::string query_param(const std::string &key,
                          const std::string &default_value = "") const;

private:
  /**
   * @brief 内部解析方法
   */
  bool parse(const std::string &uri);

  /**
   * @brief URL 解码
   */
  static std::string url_decode(const std::string &str);

private:
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
