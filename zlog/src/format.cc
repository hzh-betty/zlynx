#include "format.h"
#include <iostream>
#include <sstream>

namespace zlog {

thread_local threadId id_cached{};
thread_local std::string tidStr;

void MessageFormatItem::format(fmt::memory_buffer &buffer, const LogMessage &msg) {
    fmt::format_to(std::back_inserter(buffer), "{}", msg.payload_);
}

void LevelFormatItem::format(fmt::memory_buffer &buffer, const LogMessage &msg) {
    std::string levelstr = LogLevel::toString(msg.level_);
    fmt::format_to(std::back_inserter(buffer), "{}", levelstr);
}

TimeFormatItem::TimeFormatItem(std::string timeFormat) : timeFormat_(std::move(timeFormat)) {}

void TimeFormatItem::format(fmt::memory_buffer &buffer, const LogMessage &msg) {
    thread_local std::string cached_time;
    thread_local time_t last_cached = 0;

    if (last_cached != msg.curtime_) {
        struct tm lt{};
#ifdef _WIN32
        localtime_s(&lt, &msg.curtime_);
#else
        localtime_r(&msg.curtime_, &lt);
#endif

        // 使用临时缓冲区
        char buf[64];
        const size_t len = strftime(buf, sizeof(buf), timeFormat_.c_str(), &lt);
        if (len > 0) {
            cached_time.assign(buf, len);
            last_cached = msg.curtime_;
        } else {
            cached_time = "InvalidTime";
        }
    }
    fmt::format_to(std::back_inserter(buffer), "{}", cached_time);
}

void FileFormatItem::format(fmt::memory_buffer &buffer, const LogMessage &msg) {
    fmt::format_to(std::back_inserter(buffer), "{}", msg.file_);
}

void LineFormatItem::format(fmt::memory_buffer &buffer, const LogMessage &msg) {
    fmt::format_to(std::back_inserter(buffer), "{}", msg.line_);
}

void ThreadIdFormatItem::format(fmt::memory_buffer &buffer, const LogMessage &msg) {
    // 当缓存的ID与当前消息ID不同时才更新
    if (id_cached != msg.tid_) {
        id_cached = msg.tid_;
        std::stringstream ss;
        ss << id_cached;
        tidStr = ss.str();
    }

    fmt::format_to(std::back_inserter(buffer), "{}", tidStr);
}

void LoggerFormatItem::format(fmt::memory_buffer &buffer, const LogMessage &msg) {
    fmt::format_to(std::back_inserter(buffer), "{}", msg.loggerName_);
}

void TabFormatItem::format(fmt::memory_buffer &buffer, const LogMessage &msg) {
    (void)msg; // 避免未使用参数警告
    fmt::format_to(std::back_inserter(buffer), "{}", "\t");
}

void NLineFormatItem::format(fmt::memory_buffer &buffer, const LogMessage &msg) {
    (void)msg; // 避免未使用参数警告
    fmt::format_to(std::back_inserter(buffer), "{}", "\n");
}

OtherFormatItem::OtherFormatItem(std::string str) : str_(std::move(str)) {}

void OtherFormatItem::format(fmt::memory_buffer &buffer, const LogMessage &msg) {
    (void)msg; // 避免未使用参数警告
    fmt::format_to(std::back_inserter(buffer), "{}", str_);
}

Formatter::Formatter(std::string pattern) : pattern_(std::move(pattern)) {
    if (!parsePattern()) {
        ;
    }
}

void Formatter::format(fmt::memory_buffer &buffer, const LogMessage &msg) const {
    for (auto &item : items_) {
        item->format(buffer, msg);
    }
}

bool Formatter::parsePattern() {
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
            std::cerr << "%之后没有格式化字符" << std::endl;
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
                std::cerr << "未找到匹配的子规则字符 }" << std::endl;
                return false;
            }
            pos++;
        }

        // 6. 插入对应的key与value
        fmt_order.emplace_back(key, val);

        key.clear();
        val.clear();
    }

    // 7. 添加对应的格式化子对象
    for (auto &item : fmt_order) {
        items_.push_back(createItem(item.first, item.second));
    }

    return true;
}

FormatItem::prt Formatter::createItem(const std::string &key, const std::string &val) {
    if (key == "d") {
        if (!val.empty()) {
            return std::make_shared<TimeFormatItem>(val);
        } else {
            // 单独%d采用默认格式输出
            return std::make_shared<TimeFormatItem>();
        }
    } else if (key == "t")
        return std::make_shared<ThreadIdFormatItem>();
    else if (key == "c")
        return std::make_shared<LoggerFormatItem>();
    else if (key == "f")
        return std::make_shared<FileFormatItem>();
    else if (key == "l")
        return std::make_shared<LineFormatItem>();
    else if (key == "p")
        return std::make_shared<LevelFormatItem>();
    else if (key == "T")
        return std::make_shared<TabFormatItem>();
    else if (key == "m")
        return std::make_shared<MessageFormatItem>();
    else if (key == "n")
        return std::make_shared<NLineFormatItem>();

    if (!key.empty()) {
        std::cerr << "没有对应的格式化字符: %" << key << std::endl;
        abort();
    }
    return std::make_shared<OtherFormatItem>(val);
}

} // namespace zlog
