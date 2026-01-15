#ifndef ZHTTP_RADIX_TREE_H_
#define ZHTTP_RADIX_TREE_H_

#include "http_common.h"
#include "route_handler.h"

#include <algorithm>
#include <memory>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

namespace zhttp {

/**
 * @brief 基数树节点类型
 * 优先级: STATIC > PARAM > CATCH_ALL
 */
enum class NodeType : uint8_t {
  STATIC = 0,   // 静态文本节点
  PARAM = 1,    // 参数节点 (:id)
  CATCH_ALL = 2 // 通配符节点 (*)
};

/**
 * @brief 路由处理器包装类
 */
class RouteHandlerWrapper {
public:
  RouteHandlerWrapper() = default;

  RouteHandlerWrapper(RouterCallback callback)
      : callback_(std::move(callback)) {}

  RouteHandlerWrapper(RouteHandler::ptr handler)
      : handler_(std::move(handler)) {}

  void operator()(const HttpRequest::ptr &request,
                  HttpResponse &response) const {
    if (callback_) {
      callback_(request, response);
    } else if (handler_) {
      handler_->handle(request, response);
    }
  }

  explicit operator bool() const { return callback_ || handler_; }

private:
  RouterCallback callback_;
  RouteHandler::ptr handler_;
};

// 前向声明
class RadixNode;
using RadixNodePtr = std::shared_ptr<RadixNode>;

/**
 * @brief 正则路由条目（挂载在节点上）
 */
struct NodeRegexRoute {
  std::regex regex;                     // 编译后的正则
  std::string pattern;                  // 原始正则模式
  std::vector<std::string> param_names; // 捕获组参数名
  std::unordered_map<HttpMethod, RouteHandlerWrapper> handlers;
};

/**
 * @brief 基数树节点
 * 统一处理动态路由和正则路由
 */
class RadixNode {
public:
  using MethodHandlers = std::unordered_map<HttpMethod, RouteHandlerWrapper>;

  RadixNode() = default;
  explicit RadixNode(const std::string &path, NodeType type = NodeType::STATIC)
      : path_(path), type_(type) {}

  // 节点路径片段
  std::string path_;

  // 节点类型
  NodeType type_ = NodeType::STATIC;

  // 参数名（仅当 type_ == PARAM 时有效）
  std::string param_name_;

  // 子节点列表（按优先级排序：STATIC > PARAM > CATCH_ALL）
  std::vector<RadixNodePtr> children_;

  // 动态路由处理器（按HTTP方法）
  MethodHandlers handlers_;

  // 🆕 正则路由桶：共享相同前缀的正则都在这里
  std::vector<NodeRegexRoute> regex_routes_;

  // 是否为终端节点（有处理器）
  bool is_leaf() const { return !handlers_.empty(); }

  // 是否有正则路由
  bool has_regex() const { return !regex_routes_.empty(); }

  /**
   * @brief 添加子节点（保持优先级排序）
   */
  void add_child(RadixNodePtr child) {
    auto it =
        std::lower_bound(children_.begin(), children_.end(), child,
                         [](const RadixNodePtr &a, const RadixNodePtr &b) {
                           return static_cast<uint8_t>(a->type_) <
                                  static_cast<uint8_t>(b->type_);
                         });
    children_.insert(it, child);
  }

  /**
   * @brief 查找静态子节点（精确匹配）
   */
  RadixNodePtr find_static_child(const std::string &segment) const {
    for (const auto &child : children_) {
      if (child->type_ == NodeType::STATIC && child->path_ == segment) {
        return child;
      }
    }
    return nullptr;
  }

  /**
   * @brief 查找参数子节点
   */
  RadixNodePtr find_param_child() const {
    for (const auto &child : children_) {
      if (child->type_ == NodeType::PARAM) {
        return child;
      }
    }
    return nullptr;
  }

  /**
   * @brief 查找通配符子节点
   */
  RadixNodePtr find_catch_all_child() const {
    for (const auto &child : children_) {
      if (child->type_ == NodeType::CATCH_ALL) {
        return child;
      }
    }
    return nullptr;
  }
};

/**
 * @brief 路由匹配上下文
 */
struct RouteMatchContext {
  bool found = false;
  RouteHandlerWrapper handler;
  std::unordered_map<std::string, std::string> params;

  enum class MatchType { NONE, DYNAMIC, REGEX } match_type = MatchType::NONE;
};

/**
 * @brief 基数树路由器
 * 统一处理动态路由和正则路由
 */
class RadixTree {
public:
  RadixTree() : root_(std::make_shared<RadixNode>()) {}

  /**
   * @brief 插入动态路由
   */
  void insert(HttpMethod method, const std::string &path,
              RouteHandlerWrapper handler);

  /**
   * @brief 插入正则路由（按前缀分桶）
   */
  void insert_regex(HttpMethod method, const std::string &pattern,
                    const std::vector<std::string> &param_names,
                    RouteHandlerWrapper handler);

  /**
   * @brief 统一查找（动态路由优先，然后正则路由）
   */
  RouteMatchContext find(const std::string &path, HttpMethod method) const;

  /**
   * @brief 获取根节点
   */
  RadixNodePtr root() const { return root_; }

private:
  /**
   * @brief 分割路径为片段
   */
  std::vector<std::string> split_path(const std::string &path) const;

  /**
   * @brief 解析路径片段类型
   */
  std::pair<NodeType, std::string> parse_segment(const std::string &seg) const;

  /**
   * @brief 提取正则表达式的静态前缀
   */
  std::string extract_static_prefix(const std::string &pattern) const;

  /**
   * @brief 根据前缀路径找到或创建节点
   */
  RadixNodePtr find_or_create_prefix_node(const std::string &prefix);

  /**
   * @brief 收集前缀路径上的所有节点（用于正则匹配）
   */
  void collect_prefix_nodes(const RadixNodePtr &node,
                            const std::vector<std::string> &segments,
                            size_t index,
                            std::vector<RadixNodePtr> &nodes) const;

  /**
   * @brief 递归匹配动态路由
   */
  bool match_dynamic(const RadixNodePtr &node,
                     const std::vector<std::string> &segments, size_t index,
                     RouteMatchContext &ctx, HttpMethod method) const;

  /**
   * @brief 在路径节点上匹配正则路由
   */
  bool match_regex_on_path(const std::string &full_path, HttpMethod method,
                           const std::vector<RadixNodePtr> &path_nodes,
                           RouteMatchContext &ctx) const;

  RadixNodePtr root_;
};

} // namespace zhttp

#endif // ZHTTP_RADIX_TREE_H_
