#ifndef ZHTTP_INTERNAL_RADIX_TREE_H_
#define ZHTTP_INTERNAL_RADIX_TREE_H_

#include "zhttp/http_common.h"
#include "zhttp/router/route_handler.h"
#include <memory>
#include <regex>
#include <unordered_map>
#include <vector>

namespace zhttp {
struct RouteMatchContext {
    bool found = false;
    std::string route_id;
    RouterCallback handler;
    std::unordered_map<std::string, std::string> params;
    enum class MatchType { NONE, DYNAMIC, REGEX } match_type = MatchType::NONE;
};

/** 树节点拥有子节点；参数名称属于路由记录，拓扑仅保存匹配类型。 */
class RadixTree {
  public:
    void insert(HttpMethod method, const std::string &path, RouterCallback handler);
    void insert_regex(HttpMethod method, const std::string &pattern,
                      const std::vector<std::string> &names, RouterCallback handler);
    RouteMatchContext find(const std::string &path, HttpMethod method) const;

  private:
    enum class NodeType { STATIC, PARAM, CATCH_ALL };
    struct Route {
        RouterCallback handler;
        std::string pattern;
        std::vector<std::string> names;
    };
    struct RegexRoute {
        std::regex expression;
        std::string pattern;
        std::unordered_map<HttpMethod, Route> routes;
    };
    struct Node {
        NodeType type = NodeType::STATIC;
        std::string path;
        std::vector<std::unique_ptr<Node>> children;
        std::unordered_map<HttpMethod, Route> routes;
        std::vector<RegexRoute> regex_routes;
        Node *child(NodeType type, const std::string &path = "") const;
        Node *ensure_child(NodeType type, const std::string &path);
    };
    static std::vector<std::string> split_path(const std::string &path);
    static std::string regex_prefix(const std::string &pattern);
    static void assign_match(const Route &route, const std::vector<std::string> &values,
                             RouteMatchContext &match, RouteMatchContext::MatchType type);
    static bool match_dynamic(const Node &node, const std::vector<std::string> &segments,
                              size_t index, HttpMethod method,
                              std::vector<std::string> &values, RouteMatchContext &match);
    Node root_;
};
} // namespace zhttp
#endif
