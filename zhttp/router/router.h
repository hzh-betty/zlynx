#pragma once
#include "zhttp/http_common.h"
#include "zhttp/router/route_handler.h"
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
namespace zhttp {
struct RouteMatch {
    enum class MatchType { NONE, STATIC, DYNAMIC, REGEX, REDIRECT };
    bool found = false;
    HttpHandler handler;
    std::unordered_map<std::string, std::string> params;
    std::string route_id;
    MatchType match_type = MatchType::NONE;
};

class Router {
  public:
    Router();
    ~Router();
    Router(const Router &) = delete;
    Router &operator=(const Router &) = delete;
    void add_route(HttpMethod method, const std::string &path,
                   RouterCallback callback);
    void add_route(HttpMethod method, const std::string &path,
                   RouteHandler::ptr handler);
    void add_regex_route(HttpMethod method, const std::string &pattern,
                         const std::vector<std::string> &params,
                         RouterCallback callback);
    void add_regex_route(HttpMethod method, const std::string &pattern,
                         const std::vector<std::string> &params,
                         RouteHandler::ptr handler);
    void get(const std::string &path, RouterCallback handler);
    void get(const std::string &path, RouteHandler::ptr handler);
    void post(const std::string &path, RouterCallback handler);
    void post(const std::string &path, RouteHandler::ptr handler);
    void put(const std::string &path, RouterCallback handler);
    void put(const std::string &path, RouteHandler::ptr handler);
    void del(const std::string &path, RouterCallback handler);
    void del(const std::string &path, RouteHandler::ptr handler);
    void set_homepage(const std::string &homepage);
    const std::string &homepage() const { return homepage_; }
    RouteMatch match(const std::string &path, HttpMethod method);
    void freeze() { frozen_ = true; }

  private:
    void check_mutable() const {
        if (frozen_)
            throw std::logic_error("Router configuration is frozen");
    }
    bool is_dynamic_path(const std::string &path) const;
    bool should_redirect_to_homepage(const std::string &path,
                                     HttpMethod method) const;
    std::string normalize_homepage(const std::string &homepage) const;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string homepage_;
    bool frozen_ = false;
};
} // namespace zhttp
