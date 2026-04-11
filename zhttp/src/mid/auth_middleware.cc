#include "zhttp/mid/auth_middleware.h"

#include <unordered_set>

#include "zhttp/http_common.h"
#include "zhttp/session.h"

namespace zhttp {
namespace mid {

AuthenticationMiddleware::AuthenticationMiddleware(
    AuthenticationMiddleware::Options options)
    : options_(std::move(options)) {}

bool AuthenticationMiddleware::before(const HttpRequest::ptr &request,
                                      HttpResponse &response) {
    // Session / Bearer 采用“或”关系：任意一种命中即放行。
    const bool by_session = is_authenticated_by_session(request);
    const bool by_bearer = is_authenticated_by_bearer_token(request);

    if (by_session || by_bearer) {
        return true;
    }

    // 两种认证都失败时，返回 401（或自定义处理），并中断后续链路。
    if (options_.unauthorized_handler) {
        options_.unauthorized_handler(response);
    } else {
        response.status(HttpStatus::UNAUTHORIZED).text("Unauthorized");
    }
    return false;
}

void AuthenticationMiddleware::after(const HttpRequest::ptr &, HttpResponse &) {
}

bool AuthenticationMiddleware::is_authenticated_by_session(
    const HttpRequest::ptr &request) const {
    if (!options_.use_session) {
        return false;
    }

    // 未注入 Session 视为未认证。
    auto session = request->session();
    if (!session) {
        return false;
    }

    // 约定：登录态字段非空即代表认证通过。
    return !session->get(options_.session_auth_key).empty();
}

bool AuthenticationMiddleware::is_authenticated_by_bearer_token(
    const HttpRequest::ptr &request) const {
    // 未启用 token 或未提供校验器时，直接判失败。
    if (!options_.use_bearer_token || !options_.token_validator) {
        return false;
    }

    // Authorization 至少要包含前缀。
    const std::string authorization = request->header("Authorization");
    if (authorization.size() < options_.bearer_prefix.size()) {
        return false;
    }

    // 要求严格匹配前缀，避免误判其他认证方案。
    if (authorization.compare(0, options_.bearer_prefix.size(),
                              options_.bearer_prefix) != 0) {
        return false;
    }

    // 裁剪 token 两端空白后再校验，兼容客户端额外空格。
    std::string token = authorization.substr(options_.bearer_prefix.size());
    trim(token);
    if (token.empty()) {
        return false;
    }

    // token 是否有效由调用方注入的策略函数决定。
    return options_.token_validator(token, request);
}

RoleAuthorizationMiddleware::RoleAuthorizationMiddleware(
    std::vector<std::string> required_roles,
    RoleAuthorizationMiddleware::Options options)
    : options_(std::move(options)) {
    // 启动阶段先归一化 required roles，避免请求路径上重复处理。
    required_roles_.reserve(required_roles.size());
    for (const auto &role : required_roles) {
        const std::string normalized = normalize_role(role);
        if (!normalized.empty()) {
            required_roles_.push_back(normalized);
        }
    }
}

bool RoleAuthorizationMiddleware::before(const HttpRequest::ptr &request,
                                         HttpResponse &response) {
    // 没有配置 required roles 时默认放行。
    if (required_roles_.empty()) {
        return true;
    }

    // 解析当前请求角色并执行匹配策略。
    const std::vector<std::string> roles = resolve_request_roles(request);
    if (has_authorization(roles)) {
        return true;
    }

    // 授权失败返回 403（或自定义处理）。
    if (options_.forbidden_handler) {
        options_.forbidden_handler(response);
    } else {
        response.status(HttpStatus::FORBIDDEN).text("Forbidden");
    }
    return false;
}

void RoleAuthorizationMiddleware::after(const HttpRequest::ptr &,
                                        HttpResponse &) {}

std::vector<std::string> RoleAuthorizationMiddleware::resolve_request_roles(
    const HttpRequest::ptr &request) const {
    // 优先使用外部注入的角色解析器，便于接入 JWT/网关透传角色等场景。
    if (options_.role_resolver) {
        // 对外部返回值做统一归一化：
        // - trim 去首尾空白
        // - 根据 case_sensitive 决定是否转小写
        // - 丢弃空角色，避免后续匹配噪声
        std::vector<std::string> roles = options_.role_resolver(request);
        std::vector<std::string> normalized;
        normalized.reserve(roles.size());
        for (const auto &role : roles) {
            const std::string role_name = normalize_role(role);
            if (!role_name.empty()) {
                normalized.push_back(role_name);
            }
        }
        return normalized;
    }

    // 默认回退到 Session 角色字段。
    auto session = request->session();
    if (!session) {
        return {};
    }

    // Session 里约定角色字符串格式为："role1,role2,role3"。
    // 这里仅负责按分隔符切分；具体清洗交给 normalize_role 统一处理。
    const std::string raw_roles = session->get(options_.session_roles_key);
    std::vector<std::string> roles = split_string(raw_roles, ',');
    std::vector<std::string> normalized;
    normalized.reserve(roles.size());
    for (const auto &role : roles) {
        const std::string role_name = normalize_role(role);
        if (!role_name.empty()) {
            normalized.push_back(role_name);
        }
    }
    return normalized;
}

bool RoleAuthorizationMiddleware::has_authorization(
    const std::vector<std::string> &roles) const {
    // 无 required roles 视为无需授权约束。
    if (required_roles_.empty()) {
        return true;
    }

    // 有授权约束但请求无任何角色，直接失败。
    if (roles.empty()) {
        return false;
    }

    // 先放入集合去重，后续匹配统一走 O(1) 查询。
    std::unordered_set<std::string> role_set;
    role_set.reserve(roles.size());
    for (const auto &role : roles) {
        role_set.insert(role);
    }

    // require_all=true：必须包含全部 required roles，任一缺失即失败。
    if (options_.require_all) {
        for (const auto &required : required_roles_) {
            if (role_set.find(required) == role_set.end()) {
                return false;
            }
        }
        return true;
    }

    // require_all=false：命中任一 required role 即通过，全部未命中才失败。
    for (const auto &required : required_roles_) {
        if (role_set.find(required) != role_set.end()) {
            return true;
        }
    }
    return false;
}

std::string
RoleAuthorizationMiddleware::normalize_role(const std::string &role) const {
    // 第一步：去首尾空白，规避 " admin " 这类输入差异。
    std::string trimmed = role;
    trim(trimmed);
    if (trimmed.empty()) {
        return trimmed;
    }

    // 第二步：若不区分大小写，则统一转小写。
    // 与构造阶段对 required_roles_ 的归一化保持一致，确保比较稳定。
    if (!options_.case_sensitive) {
        return to_lower(trimmed);
    }

    return trimmed;
}

} // namespace mid
} // namespace zhttp