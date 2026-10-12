#include "router/radix_tree.h"
#include <algorithm>
#include <iterator>

namespace zhttp {
RadixTree::Node *RadixTree::Node::child(NodeType type,
                                        std::string_view path) const {
    for (const auto &node : children)
        if (node->type == type &&
            (type != NodeType::STATIC || node->path == path))
            return node.get();
    return nullptr;
}

RadixTree::Node *RadixTree::Node::ensure_child(NodeType type,
                                               std::string_view path) {
    if (auto *node = child(type, path))
        return node;
    auto node = std::make_unique<Node>();
    node->type = type;
    node->path = path;
    auto *result = node.get();
    children.push_back(std::move(node));
    return result;
}

std::vector<std::string_view> RadixTree::split_path(std::string_view path) {
    std::vector<std::string_view> segments;
    while (!path.empty()) {
        const auto slash = path.find('/');
        const auto segment = path.substr(0, slash);
        if (!segment.empty())
            segments.push_back(segment);
        if (slash == std::string_view::npos)
            break;
        path.remove_prefix(slash + 1);
    }
    return segments;
}

std::string RadixTree::regex_prefix(const std::string &pattern) {
    // 只使用首个正则元字符之前的完整目录段分桶，避免用不确定的后缀排除候选。
    auto end = pattern.find_first_of("([.*+?{\\^$|");
    std::string prefix = pattern.substr(0, end);
    const auto slash = prefix.rfind('/');
    if (slash != std::string::npos && slash + 1 < prefix.size())
        prefix.resize(slash + 1);
    return prefix;
}

void RadixTree::insert(HttpMethod method, const std::string &path,
                       RouterCallback handler) {
    Node *node = &root_;
    Route route{std::move(handler), path, {}};
    for (const auto &segment : split_path(path)) {
        const auto type = segment.front() == ':'   ? NodeType::PARAM
                          : segment.front() == '*' ? NodeType::CATCH_ALL
                                                   : NodeType::STATIC;
        if (type != NodeType::STATIC)
            route.names.emplace_back(segment.substr(1));
        node = node->ensure_child(type, segment);
    }
    node->routes[method] = std::move(route);
}

void RadixTree::insert_regex(HttpMethod method, const std::string &pattern,
                             const std::vector<std::string> &names,
                             RouterCallback handler) {
    Node *node = &root_;
    const auto prefix = regex_prefix(pattern);
    for (const auto &segment : split_path(prefix))
        node = node->ensure_child(NodeType::STATIC, segment);
    for (auto &entry : node->regex_routes)
        if (entry.pattern == pattern) {
            entry.routes[method] = Route{std::move(handler), pattern, names};
            return;
        }
    RegexRoute entry{std::regex(pattern), pattern, {}};
    entry.routes[method] = Route{std::move(handler), pattern, names};
    node->regex_routes.push_back(std::move(entry));
}

void RadixTree::assign_match(const Route &route,
                             const std::vector<std::string_view> &values,
                             RouteMatchContext &match,
                             RouteMatchContext::MatchType type) {
    match.found = true;
    match.handler = route.handler;
    match.route_id = route.pattern;
    match.match_type = type;
    for (size_t i = 0; i < values.size() && i < route.names.size(); ++i) {
        if (type == RouteMatchContext::MatchType::REGEX)
            match.params[route.names[i]] = values[i];
        else if (!route.names[i].empty())
            match.params.emplace(route.names[i], values[i]);
    }
}

bool RadixTree::match_dynamic(const Node &node,
                              const std::vector<std::string_view> &segments,
                              size_t index, HttpMethod method,
                              std::vector<std::string_view> &values,
                              RouteMatchContext &match) {
    if (index == segments.size()) {
        auto it = node.routes.find(method);
        if (it == node.routes.end())
            return false;
        assign_match(it->second, values, match,
                     RouteMatchContext::MatchType::DYNAMIC);
        return true;
    }

    // 静态分支只有完整匹配且方法命中才获胜；深层失败须回退到参数及通配分支。
    if (auto *child = node.child(NodeType::STATIC, segments[index]))
        if (match_dynamic(*child, segments, index + 1, method, values, match))
            return true;
    if (auto *child = node.child(NodeType::PARAM)) {
        values.push_back(segments[index]);
        if (match_dynamic(*child, segments, index + 1, method, values, match))
            return true;

        // 撤销当前分支的捕获，防止后续候选继承失败匹配的参数。
        values.pop_back();
    }
    if (auto *child = node.child(NodeType::CATCH_ALL)) {
        auto it = child->routes.find(method);
        if (it != child->routes.end()) {
            std::string remainder(segments[index]);
            for (size_t i = index + 1; i < segments.size(); ++i) {
                remainder.push_back('/');
                remainder.append(segments[i]);
            }
            values.push_back(remainder);
            assign_match(it->second, values, match,
                         RouteMatchContext::MatchType::DYNAMIC);
            values.pop_back();
            return true;
        }
    }
    return false;
}

RouteMatchContext RadixTree::find(const std::string &path,
                                  HttpMethod method) const {
    RouteMatchContext match;
    const auto segments = split_path(path);
    std::vector<std::string_view> values;
    if (match_dynamic(root_, segments, 0, method, values, match)) {
        match.match_type = RouteMatchContext::MatchType::DYNAMIC;
        return match;
    }
    std::vector<const Node *> prefixes{&root_};
    for (const auto &segment : segments) {
        const auto *node = prefixes.back()->child(NodeType::STATIC, segment);
        if (!node)
            break;
        prefixes.push_back(node);
    }
    for (auto it = prefixes.rbegin(); it != prefixes.rend(); ++it)
        for (const auto &entry : (*it)->regex_routes) {
            auto route = entry.routes.find(method);
            std::smatch captures;
            if (route == entry.routes.end() ||
                !std::regex_match(path, captures, entry.expression))
                continue;
            values.clear();
            for (size_t i = 1; i < captures.size(); ++i)
                values.emplace_back(
                    path.data() +
                        std::distance(path.cbegin(), captures[i].first),
                    captures[i].length());
            assign_match(route->second, values, match,
                         RouteMatchContext::MatchType::REGEX);
            match.match_type = RouteMatchContext::MatchType::REGEX;
            return match;
        }
    return match;
}
} // namespace zhttp
