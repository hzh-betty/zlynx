#ifndef ZHTTP_HTTP_REQUEST_H_
#define ZHTTP_HTTP_REQUEST_H_

#include "http_common.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace zhttp {

class Session;
class MultipartFormData;

/**
 * @brief HTTP 请求对象
 * @details
 * 该对象表示一条已经被解析出来的 HTTP 请求。它既保存原始请求的核心字段，
 * 也承载路由、中间件等后续阶段附加的数据，例如路径参数、Session、
 * multipart 解析结果等。
 */
class HttpRequest {
public:
  using ptr = std::shared_ptr<HttpRequest>;
  // Headers 保存请求头，Params 用于查询参数、路径参数、Cookie 等键值数据。
  using Headers = std::unordered_map<std::string, std::string>;
  using Params = std::unordered_map<std::string, std::string>;
  using Json = nlohmann::json;
  using RemoteAddrResolver = std::function<std::string()>;

  HttpRequest() = default;

  /**
   * @brief 获取 HTTP 方法
    * @return 解析后的请求方法枚举
   */
  HttpMethod method() const { return method_; }

  /**
   * @brief 获取请求路径
    * @return 不含查询字符串的路径部分，例如 /users/42
   */
  const std::string &path() const { return path_; }

  /**
   * @brief 获取查询字符串
    * @return ? 后面的原始字符串，不含问号本身
   */
  const std::string &query() const { return query_; }

  /**
   * @brief 获取 HTTP 版本
    * @return 请求行中声明的协议版本
   */
  HttpVersion version() const { return version_; }

  /**
   * @brief 获取所有请求头
    * @return 头字段映射表；key 按原始形式存储
   */
  const Headers &headers() const { return headers_; }

  /**
   * @brief 获取请求体
    * @return 原始 Body 内容
   */
  const std::string &body() const { return body_; }

  /**
   * @brief 获取远端地址（例如 "127.0.0.1:12345"）
   * @note 由服务器在接收连接/请求时填充
   */
  const std::string &remote_addr() const;

  /**
   * @brief 获取所有路径参数
    * @return 路由匹配后提取出的参数表
   */
  const Params &path_params() const { return path_params_; }

  /**
   * @brief 获取所有查询参数
    * @return 从 query 字符串解析出的参数表
   */
  const Params &query_params() const { return query_params_; }

  /**
   * @brief 获取特定请求头
   * @param key 请求头名称（不区分大小写）
   * @param default_val 默认值
   * @return 请求头值
   */
  std::string header(const std::string &key,
                     const std::string &default_val = "") const;

  /**
   * @brief 获取路径参数（如 /user/:id 中的 id）
   * @param key 参数名称
   * @param default_val 默认值
   * @return 参数值
   */
  std::string path_param(const std::string &key,
                         const std::string &default_val = "") const;

  /**
   * @brief 获取查询参数
   * @param key 参数名称
   * @param default_val 默认值
   * @return 参数值
   */
  std::string query_param(const std::string &key,
                          const std::string &default_val = "") const;

  /**
   * @brief 获取 Cookie
    * @param key Cookie 名称
    * @param default_val 未命中时返回的默认值
    * @return Cookie 值
   */
  std::string cookie(const std::string &key,
                     const std::string &default_val = "") const;

  /**
   * @brief 获取所有 Cookie（惰性解析）
    * @return Cookie 键值表
   */
  const Params &cookies() const;


  /**
   * @brief 获取会话对象（通常由 SessionMiddleware 注入）
    * @return 当前请求关联的 Session；未启用会话时可能为空
   */
  std::shared_ptr<Session> session() const { return runtime_.session; }

  /**
   * @brief 设置会话对象（中间件使用）
   * @param session 要绑定到请求上的会话对象
   */
  void set_session(std::shared_ptr<Session> session) {
    runtime_.session = std::move(session);
  }


  /**
   * @brief 是否为 multipart/form-data
    * @return true 表示 Content-Type 看起来是 multipart/form-data
   */
  bool is_multipart() const;

  /**
   * @brief 解析 multipart/form-data（惰性解析，重复调用无副作用）
    * @return true 表示成功，或者当前请求本来就不是 multipart 请求
   */
  bool parse_multipart();

  /**
   * @brief 获取解析后的 multipart 数据；未解析/失败返回 nullptr
    * @return multipart 解析结果指针
   */
  const MultipartFormData *multipart() const;

  /**
   * @brief 获取 multipart 解析错误（若有）
    * @return 最近一次 multipart 解析失败的错误说明
   */
  const std::string &multipart_error() const { return runtime_.multipart_error; }


  /**
   * @brief 设置 HTTP 方法
    * @param method 请求方法
   */
  void set_method(HttpMethod method) { method_ = method; }

  /**
   * @brief 设置请求路径
    * @param path 不带 query 的路径部分
   */
  void set_path(const std::string &path) { path_ = path; }

  /**
   * @brief 设置查询字符串
    * @param query 不带问号的原始查询串
   */
  void set_query(const std::string &query) { query_ = query; }

  /**
   * @brief 设置 HTTP 版本
    * @param version 协议版本
   */
  void set_version(HttpVersion version) { version_ = version; }

  /**
   * @brief 设置请求头
   * @param key 请求头名称
   * @param value 请求头值
   */
  void set_header(const std::string &key, const std::string &value);

  /**
   * @brief 设置请求体
    * @param body 请求体内容
   */
  void set_body(const std::string &body) {
    body_ = body;
    invalidate_body_cache();
  }

  /**
   * @brief 设置请求体（移动语义）
    * @param body 请求体内容
   */
  void set_body(std::string &&body) {
    body_ = std::move(body);
    invalidate_body_cache();
  }

  /**
   * @brief 设置远端地址（服务器使用）
    * @param addr 远端地址字符串
   */
  void set_remote_addr(const std::string &addr);

  /**
   * @brief 设置远端地址（移动语义）
    * @param addr 远端地址字符串
   */
  void set_remote_addr(std::string &&addr);

  /**
   * @brief 延迟设置远端地址解析逻辑
   * @details 只有第一次访问 remote_addr() 时才真正执行 resolver。
   */
  void set_remote_addr_resolver(RemoteAddrResolver resolver);

  /**
   * @brief 设置路径参数
   * @param key 参数名称
   * @param value 参数值
   */
  void set_path_param(const std::string &key, const std::string &value);

  /**
   * @brief 解析查询字符串为参数
    * @details
    * 该函数会把 query_ 拆成 query_params_。通常由解析器在解析请求行后调用，
    * 如果业务代码手动修改了 query_，也可以再次调用重新生成参数表。
   */
  void parse_query_params();

  /**
   * @brief 是否为 Keep-Alive 连接
    * @return 是否应在当前请求处理后保持连接不断开
   */
  bool is_keep_alive() const;

  /**
   * @brief 获取 Content-Length
   * @return 内容长度，未指定返回 0
   */
  size_t content_length() const;

  /**
   * @brief 获取 Content-Type
   * @return Content-Type 请求头的值；缺失时返回空字符串
   */
  std::string content_type() const;

  /**
   * @brief 是否为 JSON 请求体
   * @return true 表示 Content-Type 为 application/json（忽略参数和大小写）
   */
  bool is_json() const;

  /**
   * @brief 解析 JSON 请求体（惰性解析）
   * @return true 表示成功，或者当前请求本来就不是 JSON 请求
   */
  bool parse_json();

  /**
   * @brief 获取解析后的 JSON 对象；未解析/失败返回 nullptr
   * @return JSON 对象指针
   */
  const Json *json() const;

  /**
   * @brief 获取 JSON 解析错误（若有）
   * @return 最近一次 JSON 解析失败的错误说明
   */
  const std::string &json_error() const { return runtime_.json_error; }

  /**
   * @brief 是否为 application/x-www-form-urlencoded
   * @return true 表示 Content-Type 是 URL 编码表单
   */
  bool is_form_urlencoded() const;

  /**
   * @brief 解析 URL 编码表单请求体（惰性解析）
   * @return true 表示成功，或者当前请求本来就不是 URL 编码表单
   */
  bool parse_form_urlencoded();

  /**
   * @brief 获取解析后的 URL 编码表单字段
   * @return 表单字段映射
   */
  const Params &form_params() const;

  /**
   * @brief 获取指定 URL 编码表单字段
   * @param key 字段名
   * @param default_val 默认值
   * @return 字段值
   */
  std::string form_param(const std::string &key,
                         const std::string &default_val = "") const;

private:
  struct RuntimeData {
    std::shared_ptr<Session> session; // 会话对象，通常由 SessionMiddleware 注入

    bool cookies_parsed = false; // Cookie 是否已经解析过
    Params cookies; // 解析后的 Cookie 键值表

    bool multipart_parsed = false; // multipart 是否已经解析过
    std::shared_ptr<MultipartFormData> multipart; // 解析后的 multipart 数据
    std::string multipart_error; // 最近一次 multipart 解析失败的错误说明

    bool json_parsed = false; // JSON 是否已经解析过
    std::shared_ptr<Json> json; // 解析后的 JSON 对象
    std::string json_error; // 最近一次 JSON 解析失败的错误说明

    bool form_parsed = false; // URL 编码表单是否已经解析过
    Params form_params; // 解析后的 URL 编码表单字段
  };

  // 按需解析 Cookie，避免不访问 Cookie 的请求也付出额外开销。
  void parse_cookies_if_needed();

  // 请求体相关解析缓存失效。
  void invalidate_body_cache();

  // 解析自请求行和请求头的基础字段。
  HttpMethod method_ = HttpMethod::UNKNOWN;
  std::string path_;
  std::string query_;
  HttpVersion version_ = HttpVersion::HTTP_1_1;
  Headers headers_;
  Headers normalized_headers_;
  std::string body_;
  mutable std::string remote_addr_;
  mutable bool remote_addr_resolved_ = true;
  mutable RemoteAddrResolver remote_addr_resolver_;

  // 路由和查询参数解析结果。
  Params path_params_;  // 路径参数
  Params query_params_; // 查询参数

  // 运行期会话与惰性解析缓存（cookie/multipart/json/form）。
  RuntimeData runtime_;
};

} // namespace zhttp

#endif // ZHTTP_HTTP_REQUEST_H_
