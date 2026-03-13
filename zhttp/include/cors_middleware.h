#ifndef ZHTTP_CORS_MIDDLEWARE_H_
#define ZHTTP_CORS_MIDDLEWARE_H_

#include "middleware.h"

#include <string>
#include <vector>

namespace zhttp {

/**
 * @brief CORS 中间件
 * @details
 * 该中间件用于统一处理跨域响应头，覆盖两类请求：
 * 1) 预检请求（OPTIONS + Origin + Access-Control-Request-Method）；
 * 2) 普通跨域请求（携带 Origin）。
 *
 * 设计目标：
 *   - 预检请求可直接在中间件短路返回；
 *   - 优先回写请求 Origin；无 Origin 时回退为 "*"；
 * - 提供白名单与自定义头部能力，便于生产环境按需收紧策略。
 */
class CorsMiddleware : public Middleware {
public:
  /**
   * @brief CORS 配置项
   */
  struct Options {
    Options()
        : allow_origins({"*"}), allow_methods({"GET", "POST", "PUT", "DELETE",
                                               "PATCH", "HEAD", "OPTIONS"}),
          allow_headers(), expose_headers(), allow_credentials(false),
          max_age(600), short_circuit_preflight(true),
          forbid_disallowed_origin_on_preflight(true), add_vary_origin(true) {}

    // 允许的 Origin 列表。包含 "*" 表示允许任意 Origin。
    std::vector<std::string> allow_origins;
    // 允许的方法列表，用于 Access-Control-Allow-Methods。
    std::vector<std::string> allow_methods;
    // 允许的请求头列表；为空时可回显 Access-Control-Request-Headers。
    std::vector<std::string> allow_headers;
    // 允许前端读取的响应头列表（Access-Control-Expose-Headers）。
    std::vector<std::string> expose_headers;

    // 是否允许携带凭证（Cookie/Authorization）。
    bool allow_credentials;
    // 预检缓存秒数；<=0 表示不回写 Access-Control-Max-Age。
    int max_age;

    // 是否在预检请求阶段直接返回，避免进入业务路由。
    bool short_circuit_preflight;
    // 预检命中“非法 Origin”时是否返回 403；false 表示仅不加 CORS 头。
    bool forbid_disallowed_origin_on_preflight;
    // 非通配场景下，是否添加 Vary: Origin 以避免缓存串扰。
    bool add_vary_origin;
  };

  explicit CorsMiddleware(Options options = Options());

  /**
   * @brief 前置处理：识别并可短路预检请求
   * @return true 继续链路；false 中断链路（已写好响应）
   */
  bool before(const HttpRequest::ptr &request, HttpResponse &response) override;

  /**
   * @brief 后置处理：为普通响应补齐 CORS 头
   */
  void after(const HttpRequest::ptr &request, HttpResponse &response) override;

private:
  /**
   * @brief 判断是否为预检请求
   */
  bool is_preflight_request(const HttpRequest::ptr &request) const;

  /**
   * @brief 判断 Origin 是否在允许列表中
   */
  bool is_origin_allowed(const std::string &origin) const;

  /**
   * @brief 解析预检请求的 Access-Control-Allow-Origin 值
   * @details 请求里带 Origin 时优先回显；请求里无 Origin 时回退为 "*"。
   */
  std::string resolve_allow_origin(const std::string &origin) const;

  /**
   * @brief 添加 Vary 响应头的值
   * @param response 响应对象
   * @param value 待添加的值
   */
  void append_vary_value(HttpResponse &response,
                         const std::string &value) const;

  /**
   * @brief 应用普通 CORS 头（适用于预检和非预检请求）
   * @details 包括 Access-Control-Allow-Origin / Allow-Credentials /
   * Expose-Headers 等通用头。
   *
   * 说明：
   * - 若 Origin 不在白名单中，此函数不写任何 CORS 头；
   * - 若开启了 add_vary_origin 且返回值不是 "*"，会自动补 Vary: Origin。
   */
  void apply_common_cors_headers(const HttpRequest::ptr &request,
                                 HttpResponse &response) const;

private:
  Options options_;
};

} // namespace zhttp

#endif // ZHTTP_CORS_MIDDLEWARE_H_
