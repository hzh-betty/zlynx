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

/**
 * @brief 按路径段组织动态路由，并按静态前缀为正则路由分桶。
 *
 * 静态段优先于单段参数，再尝试通配段；树匹配失败后才检查正则。
 * 子节点由树拥有，参数名称保存于各方法的路由记录，避免共享拓扑串用名称。
 * 本类不加锁；修改配置时不得并发查询。
 */
class RadixTree {
  public:
    /**
     * @brief 注册动态路径，接管回调；同一拓扑和方法再次注册会覆盖旧记录。
     * @param path 冒号段匹配单个非空路径段，星号段在其节点上匹配剩余路径。
     * @pre 通配段位于末尾；路径已按上层路由规则校验。
     */
    void insert(HttpMethod method, const std::string &path,
                RouterCallback handler);

    /**
     * @brief 编译完整路径正则，并放入可确定的静态前缀桶。
     * @param names 按捕获组顺序命名；只保存名称与捕获组同时存在的部分。
     * @throws std::regex_error 正则表达式非法。
     */
    void insert_regex(HttpMethod method, const std::string &pattern,
                      const std::vector<std::string> &names,
                      RouterCallback handler);

    /**
     * @brief 查找路由但不执行回调，返回拥有参数字符串副本的结果。
     *
     * @return 未匹配时 found 为 false；正则按最长候选前缀及桶内注册顺序尝试。
     */
    RouteMatchContext find(const std::string &path, HttpMethod method) const;

  private:
    enum class NodeType { STATIC, PARAM, CATCH_ALL };

    struct Route {
        RouterCallback handler;
        std::string pattern;

        // 名称属于具体方法的路由记录，参数节点仅共享匹配位置。
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
        Node *child(NodeType type, std::string_view path = "") const;
        Node *ensure_child(NodeType type, std::string_view path);
    };

    static std::vector<std::string_view> split_path(std::string_view path);
    static std::string regex_prefix(const std::string &pattern);
    static void assign_match(const Route &route,
                             const std::vector<std::string_view> &values,
                             RouteMatchContext &match,
                             RouteMatchContext::MatchType type);
    static bool match_dynamic(const Node &node,
                              const std::vector<std::string_view> &segments,
                              size_t index, HttpMethod method,
                              std::vector<std::string_view> &values,
                              RouteMatchContext &match);
    Node root_;
};
} // namespace zhttp
#endif
