/**
 * @file async_logger.cc
 * @brief 日志器与内部异步执行器的装配。
 */

#include "async/looper.h"
#include "zlog/logger.h"

namespace zlog {

AsyncLogger::AsyncLogger(std::string logger_name,
                         const LogLevel::value limit_level,
                         const Formatter::ptr &formatter,
                         const std::vector<LogSink::ptr> &sinks,
                         AsyncType looper_type,
                         std::chrono::milliseconds milliseco)
    : Logger(std::move(logger_name), limit_level, formatter, sinks),
      looper_(new detail::AsyncLooper(
          [this](const detail::Buffer &buffer) {
              for (const auto &sink : sinks_) {
                  sink->log(buffer.begin(), buffer.readable_size());
              }
          },
          looper_type, milliseco)) {}

AsyncLogger::~AsyncLogger() = default;

void AsyncLogger::log(const char *data, const size_t len) {
    looper_->push(data, len);
}

} // namespace zlog
