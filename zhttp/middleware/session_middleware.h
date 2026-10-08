/**
 * session_middleware.h
 * session_middleware 定义。
 *
 * @author hzh-betty
 */

#ifndef ZHTTP_SESSION_MIDDLEWARE_H_
#define ZHTTP_SESSION_MIDDLEWARE_H_

#include "zhttp/middleware/middleware.h"
#include "zhttp/session.h"

namespace zhttp {
namespace mid {

/**
 * 会话中间件
 *
 * 该中间件在请求前检查是否存在有效会话，并在响应后根据需要创建或更新会话。
 */
class SessionMiddleware : public Middleware {
  public:
    /** 会话 Cookie 与按需创建策略。 */
    struct Options {
        Options()
            : cookie_name("ZHTTPSESSID"), cookie(), create_if_missing(true) {}

        std::string cookie_name;
        HttpResponse::CookieOptions cookie;
        bool create_if_missing; // 是否在请求无会话时自动创建新会话
    };

    /** 共享持有会话管理器并设置 Cookie 策略。 */
    explicit SessionMiddleware(SessionManager::ptr manager,
                               Options opt = Options());

    /** 从 Cookie 恢复或按配置创建会话并绑定上下文。 */
    bool before(HttpContext &context) override;

    /** 保存需要写回的会话并更新 Cookie。 */
    void after(HttpContext &context) override;

  private:
    SessionManager::ptr manager_;
    Options options_;
};

} // namespace mid

} // namespace zhttp

#endif // ZHTTP_SESSION_MIDDLEWARE_H_
