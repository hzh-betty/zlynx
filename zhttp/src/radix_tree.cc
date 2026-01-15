#include "radix_tree.h"
#include "zhttp_logger.h"

#include <sstream>

namespace zhttp {

std::vector<std::string> RadixTree::split_path(const std::string &path) const {
  std::vector<std::string> segments;
  std::istringstream iss(path);
  std::string segment;

  while (std::getline(iss, segment, '/')) {
    if (!segment.empty()) {
      segments.push_back(segment);
    }
  }

  return segments;
}

std::pair<NodeType, std::string>
RadixTree::parse_segment(const std::string &seg) const {
  if (seg.empty()) {
    return std::make_pair(NodeType::STATIC, seg);
  }

  if (seg[0] == ':') {
    return std::make_pair(NodeType::PARAM, seg.substr(1));
  }

  if (seg[0] == '*') {
    std::string name = seg.length() > 1 ? seg.substr(1) : "";
    return std::make_pair(NodeType::CATCH_ALL, name);
  }

  return std::make_pair(NodeType::STATIC, seg);
}

std::string RadixTree::extract_static_prefix(const std::string &pattern) const {
  std::string prefix;

  for (size_t i = 0; i < pattern.size(); ++i) {
    char c = pattern[i];

    // 遇到正则元字符停止
    if (c == '(' || c == '[' || c == '.' || c == '*' || c == '+' || c == '?' ||
        c == '{' || c == '\\' || c == '^' || c == '$' || c == '|') {
      break;
    }
    prefix += c;
  }

  // 去掉末尾不完整的路径段
  size_t last_slash = prefix.rfind('/');
  if (last_slash != std::string::npos && last_slash < prefix.length() - 1) {
    prefix = prefix.substr(0, last_slash + 1);
  }

  ZHTTP_LOG_DEBUG("Extracted prefix '{}' from pattern '{}'", prefix, pattern);
  return prefix;
}

RadixNodePtr RadixTree::find_or_create_prefix_node(const std::string &prefix) {
  if (prefix.empty() || prefix == "/") {
    return root_;
  }

  std::vector<std::string> segments = split_path(prefix);
  RadixNodePtr current = root_;

  for (const auto &seg : segments) {
    RadixNodePtr child = current->find_static_child(seg);

    if (!child) {
      child = std::make_shared<RadixNode>();
      child->type_ = NodeType::STATIC;
      child->path_ = seg;
      current->add_child(child);
    }

    current = child;
  }

  return current;
}

void RadixTree::insert(HttpMethod method, const std::string &path,
                       RouteHandlerWrapper handler) {
  ZHTTP_LOG_DEBUG("RadixTree::insert {} {}", method_to_string(method), path);

  std::vector<std::string> segments = split_path(path);
  RadixNodePtr current = root_;

  for (const auto &seg : segments) {
    std::pair<NodeType, std::string> parsed = parse_segment(seg);
    NodeType type = parsed.first;
    std::string param_name = parsed.second;

    RadixNodePtr child = nullptr;

    if (type == NodeType::STATIC) {
      child = current->find_static_child(seg);
    } else if (type == NodeType::PARAM) {
      child = current->find_param_child();
    } else if (type == NodeType::CATCH_ALL) {
      child = current->find_catch_all_child();
    }

    if (!child) {
      child = std::make_shared<RadixNode>();
      child->type_ = type;

      if (type == NodeType::STATIC) {
        child->path_ = seg;
      } else {
        child->path_ = seg;
        child->param_name_ = param_name;
      }

      current->add_child(child);
    }

    current = child;
  }

  current->handlers_[method] = std::move(handler);
  ZHTTP_LOG_DEBUG("Dynamic route registered: {} {}", method_to_string(method),
                  path);
}

void RadixTree::insert_regex(HttpMethod method, const std::string &pattern,
                             const std::vector<std::string> &param_names,
                             RouteHandlerWrapper handler) {
  ZHTTP_LOG_DEBUG("RadixTree::insert_regex {} {}", method_to_string(method),
                  pattern);

  std::string prefix = extract_static_prefix(pattern);
  RadixNodePtr node = find_or_create_prefix_node(prefix);

  for (auto &regex_route : node->regex_routes_) {
    if (regex_route.pattern == pattern) {
      regex_route.handlers[method] = std::move(handler);
      ZHTTP_LOG_DEBUG("Regex route updated: {} {} at prefix '{}'",
                      method_to_string(method), pattern, prefix);
      return;
    }
  }

  NodeRegexRoute route;
  route.regex = std::regex(pattern);
  route.pattern = pattern;
  route.param_names = param_names;
  route.handlers[method] = std::move(handler);

  node->regex_routes_.push_back(std::move(route));
  ZHTTP_LOG_DEBUG("Regex route registered: {} {} at prefix '{}'",
                  method_to_string(method), pattern, prefix);
}

RouteMatchContext RadixTree::find(const std::string &path,
                                  HttpMethod method) const {
  ZHTTP_LOG_DEBUG("RadixTree::find {} {}", method_to_string(method), path);

  RouteMatchContext ctx;
  std::vector<std::string> segments = split_path(path);

  // 1. 收集前缀路径上的所有节点（用于正则匹配）
  std::vector<RadixNodePtr> prefix_nodes;
  collect_prefix_nodes(root_, segments, 0, prefix_nodes);

  // 2. 尝试动态路由匹配（优先级最高）
  if (match_dynamic(root_, segments, 0, ctx, method)) {
    ctx.match_type = RouteMatchContext::MatchType::DYNAMIC;
    ZHTTP_LOG_DEBUG("Matched dynamic route: {}", path);
    return ctx;
  }

  // 3. 在收集到的前缀节点上尝试正则匹配
  if (match_regex_on_path(path, method, prefix_nodes, ctx)) {
    ctx.match_type = RouteMatchContext::MatchType::REGEX;
    ZHTTP_LOG_DEBUG("Matched regex route: {}", path);
    return ctx;
  }

  ZHTTP_LOG_DEBUG("No route matched: {}", path);
  return ctx;
}

void RadixTree::collect_prefix_nodes(const RadixNodePtr &node,
                                     const std::vector<std::string> &segments,
                                     size_t index,
                                     std::vector<RadixNodePtr> &nodes) const {
  // 添加当前节点（可能有正则路由）
  nodes.push_back(node);

  // 如果已处理完所有段，停止
  if (index >= segments.size()) {
    return;
  }

  const std::string &seg = segments[index];

  // 只沿着静态节点收集（正则路由的前缀都是静态的）
  RadixNodePtr static_child = node->find_static_child(seg);
  if (static_child) {
    collect_prefix_nodes(static_child, segments, index + 1, nodes);
  }
}

bool RadixTree::match_dynamic(const RadixNodePtr &node,
                              const std::vector<std::string> &segments,
                              size_t index, RouteMatchContext &ctx,
                              HttpMethod method) const {
  if (index >= segments.size()) {
    if (node->is_leaf()) {
      auto handler_it = node->handlers_.find(method);
      if (handler_it != node->handlers_.end()) {
        ctx.found = true;
        ctx.handler = handler_it->second;
        return true;
      }
    }
    return false;
  }

  const std::string &seg = segments[index];

  // 优先级1: 静态匹配
  RadixNodePtr static_child = node->find_static_child(seg);
  if (static_child) {
    if (match_dynamic(static_child, segments, index + 1, ctx, method)) {
      return true;
    }
  }

  // 优先级2: 参数匹配
  RadixNodePtr param_child = node->find_param_child();
  if (param_child) {
    std::string param_value = seg;
    if (match_dynamic(param_child, segments, index + 1, ctx, method)) {
      ctx.params[param_child->param_name_] = param_value;
      return true;
    }
  }

  // 优先级3: 通配符匹配
  RadixNodePtr catch_all = node->find_catch_all_child();
  if (catch_all) {
    std::string remaining;
    for (size_t i = index; i < segments.size(); ++i) {
      if (!remaining.empty()) {
        remaining += "/";
      }
      remaining += segments[i];
    }

    auto handler_it = catch_all->handlers_.find(method);
    if (handler_it != catch_all->handlers_.end()) {
      ctx.found = true;
      ctx.handler = handler_it->second;
      if (!catch_all->param_name_.empty()) {
        ctx.params[catch_all->param_name_] = remaining;
      }
      return true;
    }
  }

  return false;
}

bool RadixTree::match_regex_on_path(const std::string &full_path,
                                    HttpMethod method,
                                    const std::vector<RadixNodePtr> &path_nodes,
                                    RouteMatchContext &ctx) const {
  // 从最深的节点开始（最长前缀优先）
  for (auto it = path_nodes.rbegin(); it != path_nodes.rend(); ++it) {
    const RadixNodePtr &node = *it;

    if (!node->has_regex()) {
      continue;
    }

    ZHTTP_LOG_DEBUG("Checking {} regex routes at node",
                    node->regex_routes_.size());

    for (const auto &regex_route : node->regex_routes_) {
      std::smatch match;
      if (std::regex_match(full_path, match, regex_route.regex)) {
        auto handler_it = regex_route.handlers.find(method);
        if (handler_it != regex_route.handlers.end()) {
          ctx.found = true;
          ctx.handler = handler_it->second;

          for (size_t i = 0;
               i < regex_route.param_names.size() && i + 1 < match.size();
               ++i) {
            ctx.params[regex_route.param_names[i]] = match[i + 1].str();
          }

          ZHTTP_LOG_DEBUG("Regex matched: {}", regex_route.pattern);
          return true;
        }
      }
    }
  }

  return false;
}

} // namespace zhttp
