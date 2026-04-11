#include "zlog/message.h"
#include "zlog/util.h"
namespace zlog {

LogMessage::LogMessage(const LogLevel::value level, const char *file,
                       const size_t line, const char *payload,
                       const char *logger_name)
    : curtime_(Date::get_current_time()), level_(level), file_(file),
      line_(line), tid_(std::this_thread::get_id()), payload_(payload),
      logger_name_(logger_name) {}

} // namespace zlog
