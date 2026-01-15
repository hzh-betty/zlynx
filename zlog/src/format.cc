#include "format.h"

#include <iostream>
#include <sstream>

namespace zlog {

thread_local threadId id_cached{};
thread_local std::string tidStr;

void MessageFormatItem::format(fmt::memory_buffer &buffer,
                               const LogMessage &msg) {
  if (msg.payload_) {
    buffer.append(msg.payload_, msg.payload_ + strlen(msg.payload_));
  }
}

void LevelFormatItem::format(fmt::memory_buffer &buffer,
                             const LogMessage &msg) {
  std::string levelstr = LogLevel::toString(msg.level_);
  buffer.append(levelstr.data(), levelstr.data() + levelstr.size());
}

TimeFormatItem::TimeFormatItem(std::string timeFormat)
    : timeFormat_(std::move(timeFormat)) {}

void TimeFormatItem::format(fmt::memory_buffer &buffer, const LogMessage &msg) {
  // 秒级缓存优化
  thread_local time_t last_second = 0;
  thread_local char cached_time_str[64];
  thread_local size_t cached_len = 0;

  if (last_second != msg.curtime_) {
    struct tm lt {};
    localtime_r(&msg.curtime_, &lt);
    cached_len = strftime(cached_time_str, sizeof(cached_time_str),
                          timeFormat_.c_str(), &lt);
    last_second = msg.curtime_;
  }

  if (cached_len > 0) {
    buffer.append(cached_time_str, cached_time_str + cached_len);
  } else {
    // 错误处理
    const char *err = "InvalidTime";
    buffer.append(err, err + 11);
  }
}

void FileFormatItem::format(fmt::memory_buffer &buffer, const LogMessage &msg) {
  if (msg.file_) {
    buffer.append(msg.file_, msg.file_ + strlen(msg.file_));
  }
}

void LineFormatItem::format(fmt::memory_buffer &buffer, const LogMessage &msg) {
  fmt::format_to(std::back_inserter(buffer), "{}", msg.line_);
}

void ThreadIdFormatItem::format(fmt::memory_buffer &buffer,
                                const LogMessage &msg) {
  // 当缓存的ID与当前消息ID不同时才更新
  if (id_cached != msg.tid_) {
    id_cached = msg.tid_;
    std::stringstream ss;
    ss << id_cached;
    tidStr = ss.str();
  }

  buffer.append(tidStr.data(), tidStr.data() + tidStr.size());
}

void LoggerFormatItem::format(fmt::memory_buffer &buffer,
                              const LogMessage &msg) {
  if (msg.loggerName_) {
    buffer.append(msg.loggerName_, msg.loggerName_ + strlen(msg.loggerName_));
  }
}

void TabFormatItem::format(fmt::memory_buffer &buffer, const LogMessage &msg) {
  (void)msg; // 避免未使用参数警告
  buffer.push_back('\t');
}

void NLineFormatItem::format(fmt::memory_buffer &buffer,
                             const LogMessage &msg) {
  (void)msg; // 避免未使用参数警告
  buffer.push_back('\n');
}

OtherFormatItem::OtherFormatItem(std::string str) : str_(std::move(str)) {}

void OtherFormatItem::format(fmt::memory_buffer &buffer,
                             const LogMessage &msg) {
  (void)msg; // 避免未使用参数警告
  buffer.append(str_.data(), str_.data() + str_.size());
}

Formatter::Formatter(std::string pattern) : pattern_(std::move(pattern)) {
  if (!parsePattern()) {
    ;
  }
}

void Formatter::format(fmt::memory_buffer &buffer,
                       const LogMessage &msg) const {
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
  // 优化：合并相邻的 OtherFormatItem (字符串常量)
  for (size_t i = 0; i < fmt_order.size(); ++i) {
    if (fmt_order[i].first.empty()) {
      // 当前是普通字符串
      std::string combined_val = fmt_order[i].second;
      // 向后查看是否还有普通字符串
      while (i + 1 < fmt_order.size() && fmt_order[i + 1].first.empty()) {
        combined_val += fmt_order[i + 1].second;
        i++;
      }
      items_.push_back(std::make_shared<OtherFormatItem>(combined_val));
    } else {
      // 是格式化占位符
      items_.push_back(createItem(fmt_order[i].first, fmt_order[i].second));
    }
  }

  return true;
}

FormatItem::prt Formatter::createItem(const std::string &key,
                                      const std::string &val) {
  if (key == "d") {
    if (!val.empty()) {
      return std::make_shared<TimeFormatItem>(val);
    } else {
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
