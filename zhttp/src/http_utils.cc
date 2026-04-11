#include "zhttp/internal/http_utils.h"

#include "zhttp/http_common.h"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace zhttp {

std::string TimerHelper::format_http_date_gmt(std::time_t timestamp) {
    struct tm tm_value;
    char buffer[64];
    gmtime_r(&timestamp, &tm_value);
    std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT",
                  &tm_value);
    return buffer;
}

TimerHelper::SteadyTimePoint TimerHelper::steady_now() {
    return SteadyClock::now();
}

TimerHelper::Milliseconds TimerHelper::milliseconds(int64_t value) {
    return Milliseconds(value);
}

TimerHelper::Seconds TimerHelper::seconds(int64_t value) {
    return Seconds(value);
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

bool FileOperator::get_last_modified(const std::string &path,
                                     std::string &last_modified) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    last_modified = TimerHelper::format_http_date_gmt(st.st_mtime);
    return true;
}

bool FileOperator::get_etag(const std::string &path, std::string &etag) {
    // 这里采用弱 ETag：W/"size-mtime_ns"。
    // 好处是无需读文件内容做哈希，性能开销小且足够用于缓存协商。
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }

    uint64_t mtime_ns = static_cast<uint64_t>(st.st_mtime) * 1000000000ULL;
    mtime_ns += static_cast<uint64_t>(st.st_mtim.tv_nsec);

    std::ostringstream oss;
    oss << "W/\"" << static_cast<unsigned long long>(st.st_size) << "-"
        << static_cast<unsigned long long>(mtime_ns) << "\"";
    etag = oss.str();
    return true;
}

bool FileOperator::read_int_from_file(const std::string &path, int &value) {
    std::ifstream ifs(path);
    if (!ifs) {
        return false;
    }
    ifs >> value;
    return static_cast<bool>(ifs);
}

bool FileOperator::write_int_to_file(const std::string &path, int value) {
    std::ofstream ofs(path);
    if (!ofs) {
        return false;
    }
    ofs << value << std::endl;
    return static_cast<bool>(ofs);
}

} // namespace zhttp
