/**
 * @file format.cc
 * @brief format 实现。
 * @author hzh-betty
 */

#include "zlog/format.h"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace zlog {
namespace {

void append_string(fmt::memory_buffer &buffer, fmt::string_view text) {
    if (text.size() != 0) {
        buffer.append(text.data(), text.data() + text.size());
    }
}

void append_string(fmt::memory_buffer &buffer, const char *text) {
    if (text) {
        append_string(buffer, fmt::string_view(text));
    }
}

void append_time(fmt::memory_buffer &buffer, const LogMessage &msg,
                 const std::string &time_format) {
    // 保留原有的线程本地秒级缓存语义。
    thread_local time_t last_second = 0;
    thread_local char cached_time_str[64];
    thread_local size_t cached_len = 0;

    if (last_second != msg.curtime_) {
        struct tm lt{};
        localtime_r(&msg.curtime_, &lt);
        cached_len = strftime(cached_time_str, sizeof(cached_time_str),
                              time_format.c_str(), &lt);
        last_second = msg.curtime_;
    }

    if (cached_len > 0) {
        buffer.append(cached_time_str, cached_time_str + cached_len);
    } else {
        append_string(buffer, "InvalidTime");
    }
}

void append_thread_id(fmt::memory_buffer &buffer, const LogMessage &msg) {
    thread_local ThreadId id_cached{};
    thread_local std::string tid_str;
    if (id_cached != msg.tid_) {
        id_cached = msg.tid_;
        std::stringstream ss;
        ss << id_cached;
        tid_str = ss.str();
    }
    buffer.append(tid_str.data(), tid_str.data() + tid_str.size());
}

} // namespace

Formatter::Formatter(std::string pattern) : pattern_(std::move(pattern)) {
    if (!parse_pattern()) {
        throw std::invalid_argument("invalid log format pattern: " + pattern_);
    }
}

void Formatter::format(fmt::memory_buffer &buffer,
                       const LogMessage &msg) const {
    for (const auto &item : items_) {
        switch (item.key) {
        case 0:
            buffer.append(item.value.data(),
                          item.value.data() + item.value.size());
            break;
        case 'm':
            append_string(buffer, msg.payload_);
            break;
        case 'p': {
            const std::string level = LogLevel::to_string(msg.level_);
            buffer.append(level.data(), level.data() + level.size());
            break;
        }
        case 'd':
            append_time(buffer, msg, item.value);
            break;
        case 'f':
            append_string(buffer, msg.file_);
            break;
        case 'l':
            fmt::format_to(std::back_inserter(buffer), "{}", msg.line_);
            break;
        case 't':
            append_thread_id(buffer, msg);
            break;
        case 'c':
            append_string(buffer, msg.logger_name_);
            break;
        case 'T':
            buffer.push_back('\t');
            break;
        case 'n':
            buffer.push_back('\n');
            break;
        }
    }
}

bool Formatter::parse_pattern() {
    std::vector<std::pair<std::string, std::string>> fmt_order;
    size_t pos = 0;
    std::string key, val;
    const size_t n = pattern_.size();
    while (pos < n) {
        // 1. 不是%字符
        if (pattern_[pos] != '%') {
            val.push_back(pattern_[pos++]);
            continue;
        }

        // 2.是%%--转换为%字符
        if (pos + 1 < n && pattern_[pos + 1] == '%') {
            val.push_back('%');
            pos += 2;
            continue;
        }

        // 3. 如果起始不是%添加
        if (!val.empty()) {
            fmt_order.emplace_back("", val);
            val.clear();
        }

        // 4. 是%，开始处理格式化字符
        if (++pos == n) {
            return false;
        }

        key = pattern_[pos];
        pos++;

        // 5. 处理子规则字符
        if (pos < n && pattern_[pos] == '{') {
            pos++;
            while (pos < n && pattern_[pos] != '}') {
                val.push_back(pattern_[pos++]);
            }

            if (pos == n) {
                return false;
            }
            pos++;
        }

        // 6. 插入对应的key与value
        fmt_order.emplace_back(key, val);

        key.clear();
        val.clear();
    }

    // 7. 保存格式化项
    // 合并相邻的字符串常量
    for (size_t i = 0; i < fmt_order.size(); ++i) {
        if (fmt_order[i].first.empty()) {
            // 当前是普通字符串
            std::string combined_val = fmt_order[i].second;
            // 向后查看是否还有普通字符串
            while (i + 1 < fmt_order.size() && fmt_order[i + 1].first.empty()) {
                combined_val += fmt_order[i + 1].second;
                i++;
            }
            items_.push_back(create_item("", combined_val));
        } else {
            // 是格式化占位符
            items_.push_back(
                create_item(fmt_order[i].first, fmt_order[i].second));
        }
    }

    return true;
}

Formatter::Item Formatter::create_item(const std::string &key,
                                       const std::string &val) {
    if (key.empty()) {
        return {0, val};
    }
    if (key == "d") {
        return {'d', val.empty() ? kTimeFormatDefault : val};
    }
    if (key.size() == 1) {
        switch (key.front()) {
        case 't':
        case 'c':
        case 'f':
        case 'l':
        case 'p':
        case 'T':
        case 'm':
        case 'n':
            return {key.front(), ""};
        }
    }
    throw std::invalid_argument("unknown log format item: %" + key);
}

} // namespace zlog
