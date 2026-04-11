#ifndef ZHTTP_SESSION_MIDDLEWARE_H_
#define ZHTTP_SESSION_MIDDLEWARE_H_

#include "zhttp/mid/middleware.h"
#include "zhttp/session.h"

namespace zhttp {
namespace mid {

/**
 * @brief 会话中间件
 * @details
 * 该中间件在请求前检查是否存在有效会话，并在响应后根据需要创建或更新会话。
 */
class SessionMiddleware : public Middleware {
  public:
    struct Options {
        Options()
            : cookie_name("ZHTTPSESSID"), cookie(), create_if_missing(true) {}

        std::string cookie_name;
        HttpResponse::CookieOptions cookie;
        bool create_if_missing; // 是否在请求无会话时自动创建新会话
    };

    explicit SessionMiddleware(SessionManager::ptr manager,
                               Options opt = Options());

    bool before(const HttpRequest::ptr &request,
                HttpResponse &response) override;

    void after(const HttpRequest::ptr &request,
               HttpResponse &response) override;

  private:
    SessionManager::ptr manager_;
    Options options_;
};

} // namespace mid

} // namespace zhttp

#endif // ZHTTP_SESSION_MIDDLEWARE_H_
