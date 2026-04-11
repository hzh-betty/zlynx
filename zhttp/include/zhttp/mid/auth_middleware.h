#ifndef ZHTTP_AUTH_MIDDLEWARE_H_
#define ZHTTP_AUTH_MIDDLEWARE_H_

#include "zhttp/mid/middleware.h"

#include <functional>
#include <string>
#include <vector>

namespace zhttp {
namespace mid {

/**
 * @brief 认证中间件：校验请求是否已登录
 *
 * 支持两种认证来源（可单独或组合启用）：
 * - Session：检查会话中的某个键是否存在（如 user_id）
 * - Bearer Token：从 Authorization 头提取 Bearer token 并调用自定义校验函数
 *
 * 判定策略：
 * - 两种来源是“或”关系，只要任意一种通过即放行。
 * - 两种来源都未通过时，中断链路并返回未认证响应。
 */
class AuthenticationMiddleware : public Middleware {
  public:
    using TokenValidator =
        std::function<bool(const std::string &token, const HttpRequest::ptr &)>;
    using UnauthorizedHandler = std::function<void(HttpResponse &)>;

    /**
     * @brief 认证中间件配置
     */
    struct Options {
        Options()
            : use_session(true), session_auth_key("user_id"),
              use_bearer_token(false), bearer_prefix("Bearer ") {}

        bool use_session;             // 是否启用 Session 鉴权
        std::string session_auth_key; // Session 中用于标记已登录的键名

        bool use_bearer_token; // 是否启用 Bearer Token 鉴权
        std::string
            bearer_prefix; // Authorization 头中 token 前缀，默认 "Bearer "

        // 返回 true 表示 token 有效
        TokenValidator token_validator;

        // 自定义未认证响应；不设置时默认返回 401 + 文本
        UnauthorizedHandler unauthorized_handler;
    };

    explicit AuthenticationMiddleware(Options options = Options());

    /**
     * @brief 请求进入业务前执行认证
     * @return true 认证通过继续执行；false 认证失败并中断链路
     */
    bool before(const HttpRequest::ptr &request,
                HttpResponse &response) override;

    /**
     * @brief 认证中间件无后置逻辑
     */
    void after(const HttpRequest::ptr &request,
               HttpResponse &response) override;

  private:
    bool is_authenticated_by_session(const HttpRequest::ptr &request) const;
    bool
    is_authenticated_by_bearer_token(const HttpRequest::ptr &request) const;

    Options options_;
};

/**
 * @brief 角色授权中间件：校验当前用户是否具备访问所需角色
 *
 * 默认从 Session 中读取角色列表（逗号分隔字符串），例如："admin,editor"。
 * 也支持提供自定义 role_resolver，从请求中解析角色集合。
 *
 * 匹配策略：
 * - require_all=false：当前角色集合命中任一 required role 即通过。
 * - require_all=true：必须包含全部 required role 才通过。
 */
class RoleAuthorizationMiddleware : public Middleware {
  public:
    using RoleResolver =
        std::function<std::vector<std::string>(const HttpRequest::ptr &)>;
    using ForbiddenHandler = std::function<void(HttpResponse &)>;

    /**
     * @brief 授权中间件配置
     */
    struct Options {
        Options()
            : session_roles_key("roles"), require_all(false),
              case_sensitive(false) {}

        std::string
            session_roles_key; // 默认角色来源：Session 中该键保存的角色字符串
        bool
            require_all; // true=必须包含全部 required roles；false=命中任一即可
        bool case_sensitive; // 角色匹配是否区分大小写

        // 自定义角色解析器；返回当前请求拥有的角色集合
        RoleResolver role_resolver;

        // 自定义无权限响应；不设置时默认返回 403 + 文本
        ForbiddenHandler forbidden_handler;
    };

    RoleAuthorizationMiddleware(std::vector<std::string> required_roles,
                                Options options = Options());

    /**
     * @brief 请求进入业务前执行角色授权
     * @return true 授权通过继续执行；false 授权失败并中断链路
     */
    bool before(const HttpRequest::ptr &request,
                HttpResponse &response) override;

    /**
     * @brief 授权中间件无后置逻辑
     */
    void after(const HttpRequest::ptr &request,
               HttpResponse &response) override;

  private:
    /**
     * @brief 解析当前请求的角色集合（已归一化）
     * @param request 当前 HTTP 请求
     * @return 角色列表；若无角色返回空数组
     * @details
     * 解析顺序：
     * 1) 若配置了 role_resolver，则优先使用其返回值；
     * 2) 否则从 Session 的 session_roles_key 字段读取并按逗号切分。
     */
    std::vector<std::string>
    resolve_request_roles(const HttpRequest::ptr &request) const;

    /**
     * @brief 判断当前角色是否满足访问要求
     * @param roles 当前请求角色（建议已归一化）
     * @return true 表示授权通过
     * @details
     * - require_all=true：必须包含 required_roles_ 的全部元素；
     * - require_all=false：包含任一 required_roles_ 元素即可。
     */
    bool has_authorization(const std::vector<std::string> &roles) const;

    /**
     * @brief 归一化单个角色字符串
     * @param role 原始角色字符串
     * @return 归一化后的角色（去首尾空白，必要时转小写）
     */
    std::string normalize_role(const std::string &role) const;

    std::vector<std::string> required_roles_; // 访问所需角色列表（已归一化）
    Options options_;
};

} // namespace mid

} // namespace zhttp

#endif // ZHTTP_AUTH_MIDDLEWARE_H_