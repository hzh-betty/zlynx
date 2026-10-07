/**
 * @file http_utils.cc
 * @brief http_utils 实现。
 * @author hzh-betty
 */

#include "zhttp/internal/http_utils.h"

#include "zhttp/http_common.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace zhttp {

namespace {

// qvalue 使用千分整数，避免浮点和宽松数值解析接受非法输入。
int parse_encoding_quality(const std::string &value) {
    if (value.empty() || (value[0] != '0' && value[0] != '1')) return 0;
    if (value.size() == 1) return value[0] == '1' ? 1000 : 0;
    if (value[1] != '.' || value.size() > 5) return 0;
    int quality = value[0] == '1' ? 1000 : 0;
    int place = 100;
    for (size_t i = 2; i < value.size(); ++i, place /= 10) {
        if (value[i] < '0' || value[i] > '9' ||
            (value[0] == '1' && value[i] != '0')) return 0;
        quality += (value[i] - '0') * place;
    }
    return quality;
}

} // namespace

std::vector<std::string> accepted_content_encodings(
    const std::string &header, bool enable_br, bool enable_gzip) {
    // 保留默认行为：未指定编码时返回原始实体。
    if (header.empty()) return {""};
    int br = -1, gzip = -1, identity = -1, wildcard = -1;
    for (std::string item : split_string(to_lower(header), ',')) {
        const size_t semi = item.find(';');
        std::string token = item.substr(0, semi);
        trim(token);
        int *target = token == "br" ? &br : token == "gzip" ? &gzip :
                      token == "identity" ? &identity : token == "*" ? &wildcard : nullptr;
        if (!target) continue;
        int quality = 1000;
        if (semi != std::string::npos) {
            std::string parameter = item.substr(semi + 1);
            const size_t equal = parameter.find('=');
            std::string name = parameter.substr(0, equal);
            trim(name);
            std::string value = equal == std::string::npos ? "" : parameter.substr(equal + 1);
            trim(value);
            quality = name == "q" ? parse_encoding_quality(value) : 0;
        }
        // 重复项取较严格值，显式 q=0 不能被另一个重复项覆盖。
        *target = *target < 0 ? quality : std::min(*target, quality);
    }
    std::vector<std::pair<std::string, int>> ranked;
    const int br_quality = br < 0 ? wildcard : br;
    const int gzip_quality = gzip < 0 ? wildcard : gzip;
    if (enable_br && br_quality > 0) ranked.emplace_back("br", br_quality);
    if (enable_gzip && gzip_quality > 0) ranked.emplace_back("gzip", gzip_quality);
    if (identity > 0) ranked.emplace_back("", identity);
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) {
        return a.second > b.second;
    });
    std::vector<std::string> result;
    for (const auto &entry : ranked) result.push_back(entry.first);
    if (identity < 0 && wildcard != 0) result.emplace_back("");
    return result;
}


std::string format_http_date_gmt(std::time_t timestamp) {
    struct tm tm_value;
    char buffer[64];
    gmtime_r(&timestamp, &tm_value);
    std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT",
                  &tm_value);
    return buffer;
}




std::string PathOperator::normalize_prefix(const std::string &prefix) {
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

bool PathOperator::should_handle_path(const std::string &path,
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
PathOperator::map_to_relative_path(const std::string &path,
                                   const std::string &normalized_prefix) {
    if (normalized_prefix == "/") {
        return path;
    }
    if (path.size() <= normalized_prefix.size()) {
        return "/";
    }
    return path.substr(normalized_prefix.size());
}

bool PathOperator::sanitize_relative_path(const std::string &raw,
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

std::string PathOperator::join_path(const std::string &left,
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

bool FileOperator::is_regular_file(const std::string &path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISREG(st.st_mode);
}

bool FileOperator::is_directory(const std::string &path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISDIR(st.st_mode);
}

bool FileOperator::read_file(const std::string &path, std::string &content) {
    std::ifstream ifs(path, std::ios::in | std::ios::binary);
    if (!ifs) {
        return false;
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    content = oss.str();
    return true;
}

bool FileOperator::write_file_binary(const std::string &path,
                                     const std::string &content) {
    std::ofstream ofs(path.c_str(), std::ios::binary);
    if (!ofs) {
        return false;
    }
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(ofs);
}

std::string FileOperator::detect_content_type(const std::string &file_path) {
    size_t dot = file_path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= file_path.size()) {
        return get_mime_type("");
    }
    return get_mime_type(file_path.substr(dot + 1));
}


std::string FileOperator::get_etag(const struct stat &st) {
    // 保留弱 ETag 格式：W/"size-mtime_ns"。
    uint64_t mtime_ns = static_cast<uint64_t>(st.st_mtime) * 1000000000ULL;
    mtime_ns += static_cast<uint64_t>(st.st_mtim.tv_nsec);

    std::ostringstream oss;
    oss << "W/\"" << static_cast<unsigned long long>(st.st_size) << "-"
        << static_cast<unsigned long long>(mtime_ns) << "\"";
    return oss.str();
}



} // namespace zhttp
