#include "logger.h"
#include "util.h"
namespace zlog {

Logger::Logger(const char *loggerName, const LogLevel::value limitLevel,
               Formatter::ptr formatter, std::vector<LogSink::ptr> &sinks)
    : loggerName_(loggerName), limitLevel_(limitLevel),
      formatter_(std::move(formatter)), sinks_(sinks.begin(), sinks.end()) {}

void Logger::serialize(const LogLevel::value level, const char *file,
                       const size_t line, const char *data) {
  // 1. 线程本地日志消息对象，避免构造/析构开销
  thread_local LogMessage msg(LogLevel::value::DEBUG, "", 0, "", "");

  // 2. 直接赋值（快速）
  msg.curtime_ = Date::getCurrentTime();
  msg.level_ = level;
  msg.file_ = file;
  msg.line_ = line;
  msg.tid_ = std::this_thread::get_id();
  msg.payload_ = data;
  msg.loggerName_ = loggerName_;

  // 3. 线程本地格式化缓冲区，避免内存分配
  thread_local fmt::memory_buffer buffer;
  buffer.clear();

  // 4. 格式化
  formatter_->format(buffer, msg);

  // 5. 日志
  log(buffer.data(), buffer.size());
}

SyncLogger::SyncLogger(const char *loggerName, const LogLevel::value limitLevel,
                       const Formatter::ptr &formatter,
                       std::vector<LogSink::ptr> &sinks)
    : Logger(loggerName, limitLevel, formatter, sinks) {}

void SyncLogger::log(const char *data, const size_t len) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (sinks_.empty())
    return;
  for (const auto &sink : sinks_) {
    sink->log(data, len);
  }
}

AsyncLogger::AsyncLogger(const char *loggerName,
                         const LogLevel::value limitLevel,
                         const Formatter::ptr &formatter,
                         std::vector<LogSink::ptr> &sinks, AsyncType looperType,
                         std::chrono::milliseconds milliseco)
    : Logger(loggerName, limitLevel, formatter, sinks),
      looper_(std::make_shared<AsyncLooper>(
          AsyncLooper::Functor{[this](const Buffer &buf) { this->reLog(buf); }},
          looperType, milliseco)) {}

void AsyncLogger::log(const char *data, const size_t len) {
  looper_->push(data, len);
}

void AsyncLogger::reLog(const Buffer &buffer) const {
  if (sinks_.empty())
    return;
  for (auto &sink : sinks_) {
    sink->log(buffer.begin(), buffer.readAbleSize());
  }
}

LoggerBuilder::LoggerBuilder()
    : loggerType_(LoggerType::LOGGER_SYNC), limitLevel_(LogLevel::value::DEBUG),
      looperType_(AsyncType::ASYNC_SAFE),
      milliseco_(std::chrono::milliseconds(3000)) {}

void LoggerBuilder::buildLoggerType(const LoggerType loggerType) {
  loggerType_ = loggerType;
}

void LoggerBuilder::buildEnalleUnSafe() {
  looperType_ = AsyncType::ASYNC_UNSAFE;
}

void LoggerBuilder::buildLoggerName(const char *loggerName) {
  loggerName_ = loggerName;
}

void LoggerBuilder::buildLoggerLevel(LogLevel::value limitLevel) {
  limitLevel_ = limitLevel;
}

void LoggerBuilder::buildWaitTime(const std::chrono::milliseconds milliseco) {
  milliseco_ = milliseco;
}

void LoggerBuilder::buildLoggerFormatter(const std::string &pattern) {
  formatter_ = std::make_shared<Formatter>(pattern);
}

Logger::ptr LocalLoggerBuilder::build() {
  if (loggerName_ == nullptr) {
    return {};
  }
  if (formatter_.get() == nullptr) {
    formatter_ = std::make_shared<Formatter>();
  }
  if (sinks_.empty()) {
    buildLoggerSink<StdOutSink>();
  }
  if (loggerType_ == LoggerType::LOGGER_ASYNC) {
    return std::make_shared<AsyncLogger>(loggerName_, limitLevel_, formatter_,
                                         sinks_, looperType_, milliseco_);
  }
  return std::make_shared<SyncLogger>(loggerName_, limitLevel_, formatter_,
                                      sinks_);
}

LoggerManager::LoggerManager() {
  const std::unique_ptr<zlog::LocalLoggerBuilder> builder(
      new zlog::LocalLoggerBuilder());
  builder->buildLoggerName("root");
  rootLogger_ = builder->build();
  loggers_.insert({"root", rootLogger_});
}

void LoggerManager::addLogger(Logger::ptr &logger) {
  if (hasLogger(logger->getName()))
    return;
  std::unique_lock<std::mutex> lock(mutex_);
  loggers_.insert({logger->getName(), logger});
}

bool LoggerManager::hasLogger(const std::string &name) {
  std::unique_lock<std::mutex> lock(mutex_);
  const auto iter = loggers_.find(name);
  if (iter == loggers_.end()) {
    return false;
  }
  return true;
}

Logger::ptr LoggerManager::getLogger(const std::string &name) {
  std::unique_lock<std::mutex> lock(mutex_);
  const auto iter = loggers_.find(name);
  if (iter == loggers_.end()) {
    return {};
  }
  return iter->second;
}

Logger::ptr LoggerManager::rootLogger() { return rootLogger_; }

Logger::ptr GlobalLoggerBuilder::build() {
  if (loggerName_ == nullptr) {
    return {};
  }
  if (formatter_.get() == nullptr) {
    formatter_ = std::make_shared<Formatter>();
  }
  if (sinks_.empty()) {
    buildLoggerSink<StdOutSink>();
  }
  Logger::ptr logger;
  if (loggerType_ == LoggerType::LOGGER_ASYNC) {
    logger = std::make_shared<AsyncLogger>(loggerName_, limitLevel_, formatter_,
                                           sinks_, looperType_, milliseco_);
  } else {
    logger = std::make_shared<SyncLogger>(loggerName_, limitLevel_, formatter_,
                                          sinks_);
  }
  LoggerManager::getInstance().addLogger(logger);
  return logger;
}

} // namespace zlog
