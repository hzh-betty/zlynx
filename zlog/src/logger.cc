/**
 * @file logger.cc
 * @brief logger 实现。
 * @author hzh-betty
 */

#include "zlog/logger.h"

#include <ctime>


namespace zlog {

Logger::Logger(std::string logger_name, const LogLevel::value limit_level,
               Formatter::ptr formatter, const std::vector<LogSink::ptr> &sinks)
    : logger_name_(std::move(logger_name)), limit_level_(limit_level),
      formatter_(std::move(formatter)), sinks_(sinks.begin(), sinks.end()) {}

void Logger::serialize(const LogLevel::value level, const char *file,
                       const size_t line, fmt::string_view data) {
    // 1. 线程本地日志消息对象，避免构造/析构开销
    thread_local LogMessage msg(LogLevel::value::DEBUG, "", 0, "", "");

    // 2. 直接赋值（快速）
    msg.curtime_ = std::time(nullptr);
    msg.level_ = level;
    msg.file_ = file;
    msg.line_ = line;
    msg.tid_ = std::this_thread::get_id();
    msg.payload_ = data;
    msg.logger_name_ = fmt::string_view(logger_name_.data(), logger_name_.size());

    // 3. 线程本地格式化缓冲区，避免内存分配
    thread_local fmt::memory_buffer buffer;
    buffer.clear();

    // 4. 格式化
    formatter_->format(buffer, msg);

    // 5. 日志
    log(buffer.data(), buffer.size());
}

SyncLogger::SyncLogger(std::string logger_name,
                       const LogLevel::value limit_level,
                       const Formatter::ptr &formatter,
                       const std::vector<LogSink::ptr> &sinks)
    : Logger(std::move(logger_name), limit_level, formatter, sinks) {}

void SyncLogger::log(const char *data, const size_t len) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (sinks_.empty())
        return;
    for (const auto &sink : sinks_) {
        sink->log(data, len);
    }
}

} // namespace zlog
