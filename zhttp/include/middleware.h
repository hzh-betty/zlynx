#ifndef ZHTTP_MIDDLEWARE_H_
#define ZHTTP_MIDDLEWARE_H_

#include "http_request.h"
#include "http_response.h"

#include <memory>
#include <vector>

namespace zhttp {

/**
 * @brief 中间件抽象基类
 * 用户通过继承此类实现自定义中间件
 */
class Middleware {
public:
  using ptr = std::shared_ptr<Middleware>;

  virtual ~Middleware() = default;

  /**
   * @brief 请求处理前调用
   * @param request HTTP 请求对象
   * @param response HTTP 响应对象
   * @return true 继续执行后续中间件/处理器，false 中断请求处理链
   */
  virtual bool before(const HttpRequest::ptr &request,
                      HttpResponse &response) = 0;

  /**
   * @brief 请求处理后调用
   * @param request HTTP 请求对象
   * @param response HTTP 响应对象
   * @note 无论 handler 是否成功执行，after 都会被调用（用于清理资源等）
   */
  virtual void after(const HttpRequest::ptr &request,
                     HttpResponse &response) = 0;
};

/**
 * @brief 中间件链执行器
 */
class MiddlewareChain {
public:
  /**
   * @brief 添加中间件
   * @param middleware 中间件对象
   */
  void add(Middleware::ptr middleware);

  /**
   * @brief 获取中间件数量
   */
  size_t size() const { return middlewares_.size(); }

  /**
   * @brief 是否为空
   */
  bool empty() const { return middlewares_.empty(); }

  /**
   * @brief 执行所有中间件的 before 方法
   * @param request HTTP 请求
   * @param response HTTP 响应
   * @return true 全部通过，false 某个中间件中断了请求
   */
  bool execute_before(const HttpRequest::ptr &request, HttpResponse &response);

  /**
   * @brief 执行所有中间件的 after 方法（逆序执行）
   * @param request HTTP 请求
   * @param response HTTP 响应
   */
  void execute_after(const HttpRequest::ptr &request, HttpResponse &response);

  /**
   * @brief 获取中间件列表
   */
  const std::vector<Middleware::ptr> &middlewares() const {
    return middlewares_;
  }

private:
  std::vector<Middleware::ptr> middlewares_;
  size_t executed_count_ = 0; // 记录 before 执行到的位置，用于 after 逆序
};

} // namespace zhttp

#endif // ZHTTP_MIDDLEWARE_H_
