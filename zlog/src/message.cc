/**
 * @file message.cc
 * @brief message 实现。
 * @author hzh-betty
 */

#include "zlog/message.h"

#include <ctime>

namespace zlog {

LogMessage::LogMessage(const LogLevel::value level, const char *file,
                       const size_t line, fmt::string_view payload,
                       fmt::string_view logger_name)
    : curtime_(std::time(nullptr)), level_(level), file_(file), line_(line),
      tid_(std::this_thread::get_id()), payload_(payload),
      logger_name_(logger_name) {}

LogMessage::LogMessage(const LogLevel::value level, const char *file,
                       const size_t line, const char *payload,
                       const char *logger_name)
    : LogMessage(level, file, line,
                 payload ? fmt::string_view(payload) : fmt::string_view(),
                 logger_name ? fmt::string_view(logger_name)
                             : fmt::string_view()) {}

} // namespace zlog
