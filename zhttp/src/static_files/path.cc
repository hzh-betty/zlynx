/**
 * http_utils.cc
 * http_utils 实现。
 *
 * @author hzh-betty
 */

#include "static_files/path.h"

#include "zhttp/http_common.h"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace zhttp {

std::string static_path::normalize_prefix(const std::string &prefix) {
    // 统一前缀格式，减少调用方分支判断复杂度。
    if (prefix.empty() || prefix == "/") {
        return "/";
    }

    std::string value = prefix;
    if (value[0] != '/') {
        value = "/" + value;
    }
    while (value.size() > 1 && value.back() == '/') {
        value.pop_back();
    }
    return value;
}

bool static_path::should_handle_path(const std::string &path,
                                      const std::string &normalized_prefix) {
    if (normalized_prefix == "/") {
        return !path.empty() && path[0] == '/';
    }

    if (path.size() < normalized_prefix.size() ||
        path.compare(0, normalized_prefix.size(), normalized_prefix) != 0) {
        return false;
    }

    if (path.size() == normalized_prefix.size()) {
        return true;
    }

    return path[normalized_prefix.size()] == '/';
}

std::string
static_path::map_to_relative_path(const std::string &path,
                                   const std::string &normalized_prefix) {
    if (normalized_prefix == "/") {
        return path;
    }
    if (path.size() <= normalized_prefix.size()) {
        return "/";
    }
    return path.substr(normalized_prefix.size());
}

bool static_path::sanitize_relative_path(const std::string &raw,
                                          std::string &out) {
    // 先 URL 解码，防止 %2e%2e 这种编码形式绕过目录穿越检查。
    std::string decoded = url_decode(raw);
    std::vector<std::string> segments;
    std::string segment;

    auto flush_segment = [&segments](std::string &seg) -> bool {
        // 归一化规则：空段和 . 忽略，.. 直接拒绝，其余保留。
        if (seg.empty() || seg == ".") {
            seg.clear();
            return true;
        }
        if (seg == "..") {
            return false;
        }
        segments.push_back(seg);
        seg.clear();
        return true;
    };

    for (char c : decoded) {
        if (c == '/') {
            if (!flush_segment(segment)) {
                return false;
            }
            continue;
        }
        segment.push_back(c);
    }

    if (!flush_segment(segment)) {
        return false;
    }

    std::ostringstream oss;
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) {
            oss << '/';
        }
        oss << segments[i];
    }
    out = oss.str();
    return true;
}

std::string static_path::join_path(const std::string &left,
                                    const std::string &right) {
    if (right.empty()) {
        return left;
    }
    if (left.empty()) {
        return right;
    }
    if (left.back() == '/') {
        return left + right;
    }
    return left + "/" + right;
}

} // namespace zhttp
