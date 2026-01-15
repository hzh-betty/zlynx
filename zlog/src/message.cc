#include "message.h"
#include "util.h"
namespace zlog {

LogMessage::LogMessage(const LogLevel::value level, const char *file,
                       const size_t line, const char *payload,
                       const char *loggerName)
    : curtime_(Date::getCurrentTime()), level_(level), file_(file), line_(line),
      tid_(std::this_thread::get_id()), payload_(payload),
      loggerName_(loggerName) {}

} // namespace zlog
