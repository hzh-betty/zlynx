#include "zlog/logger.h"
#include "zlog/util.h"
namespace zlog {

Logger::Logger(const char *logger_name, const LogLevel::value limit_level,
               Formatter::ptr formatter, std::vector<LogSink::ptr> &sinks)
    : logger_name_(logger_name), limit_level_(limit_level),
      formatter_(std::move(formatter)), sinks_(sinks.begin(), sinks.end()) {}

void Logger::serialize(const LogLevel::value level, const char *file,
                       const size_t line, const char *data) {
    // 1. 线程本地日志消息对象，避免构造/析构开销
    thread_local LogMessage msg(LogLevel::value::DEBUG, "", 0, "", "");

    // 2. 直接赋值（快速）
    msg.curtime_ = Date::get_current_time();
    msg.level_ = level;
    msg.file_ = file;
    msg.line_ = line;
    msg.tid_ = std::this_thread::get_id();
    msg.payload_ = data;
    msg.logger_name_ = logger_name_;

    // 3. 线程本地格式化缓冲区，避免内存分配
    thread_local fmt::memory_buffer buffer;
    buffer.clear();

    // 4. 格式化
    formatter_->format(buffer, msg);

    // 5. 日志
    log(buffer.data(), buffer.size());
}

SyncLogger::SyncLogger(const char *logger_name,
                       const LogLevel::value limit_level,
                       const Formatter::ptr &formatter,
                       std::vector<LogSink::ptr> &sinks)
    : Logger(logger_name, limit_level, formatter, sinks) {}

void SyncLogger::log(const char *data, const size_t len) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (sinks_.empty())
        return;
    for (const auto &sink : sinks_) {
        sink->log(data, len);
    }
}

AsyncLogger::AsyncLogger(const char *logger_name,
                         const LogLevel::value limit_level,
                         const Formatter::ptr &formatter,
                         std::vector<LogSink::ptr> &sinks,
                         AsyncType looper_type,
                         std::chrono::milliseconds milliseco)
    : Logger(logger_name, limit_level, formatter, sinks),
      looper_(std::make_shared<AsyncLooper>(
          AsyncLooper::Functor{
              [this](const Buffer &buf) { this->re_log(buf); }},
          looper_type, milliseco)) {}

void AsyncLogger::log(const char *data, const size_t len) {
    looper_->push(data, len);
}

void AsyncLogger::re_log(const Buffer &buffer) const {
    if (sinks_.empty())
        return;
    for (auto &sink : sinks_) {
        sink->log(buffer.begin(), buffer.readable_size());
    }
}

LoggerBuilder::LoggerBuilder()
    : logger_type_(LoggerType::LOGGER_SYNC),
      limit_level_(LogLevel::value::DEBUG), looper_type_(AsyncType::ASYNC_SAFE),
      milliseco_(std::chrono::milliseconds(3000)) {}

void LoggerBuilder::build_logger_type(const LoggerType logger_type) {
    logger_type_ = logger_type;
}

void LoggerBuilder::build_enable_unsafe() {
    looper_type_ = AsyncType::ASYNC_UNSAFE;
}

void LoggerBuilder::build_logger_name(const char *logger_name) {
    logger_name_ = logger_name;
}

void LoggerBuilder::build_logger_level(LogLevel::value limit_level) {
    limit_level_ = limit_level;
}

void LoggerBuilder::build_wait_time(const std::chrono::milliseconds milliseco) {
    milliseco_ = milliseco;
}

void LoggerBuilder::build_logger_formatter(const std::string &pattern) {
    formatter_ = std::make_shared<Formatter>(pattern);
}

Logger::ptr LocalLoggerBuilder::build() {
    if (logger_name_ == nullptr) {
        return {};
    }
    if (formatter_.get() == nullptr) {
        formatter_ = std::make_shared<Formatter>();
    }
    if (sinks_.empty()) {
        build_logger_sink<StdOutSink>();
    }
    if (logger_type_ == LoggerType::LOGGER_ASYNC) {
        return std::make_shared<AsyncLogger>(logger_name_, limit_level_,
                                             formatter_, sinks_, looper_type_,
                                             milliseco_);
    }
    return std::make_shared<SyncLogger>(logger_name_, limit_level_, formatter_,
                                        sinks_);
}

LoggerManager::LoggerManager() {
    const std::unique_ptr<zlog::LocalLoggerBuilder> builder(
        new zlog::LocalLoggerBuilder());
    builder->build_logger_name("root");
    root_logger_ = builder->build();
    loggers_.insert({"root", root_logger_});
}

void LoggerManager::add_logger(Logger::ptr &logger) {
    if (has_logger(logger->get_name()))
        return;
    std::unique_lock<std::mutex> lock(mutex_);
    loggers_.insert({logger->get_name(), logger});
}

void LoggerManager::upsert_logger(const std::string &name, Logger::ptr logger) {
    if (!logger) {
        return;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    loggers_[name] = logger;
    if (name == "root") {
        root_logger_ = logger;
    }
}

bool LoggerManager::has_logger(const std::string &name) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto iter = loggers_.find(name);
    if (iter == loggers_.end()) {
        return false;
    }
    return true;
}

Logger::ptr LoggerManager::get_logger(const std::string &name) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto iter = loggers_.find(name);
    if (iter == loggers_.end()) {
        return {};
    }
    return iter->second;
}

Logger::ptr LoggerManager::root_logger() { return root_logger_; }

Logger::ptr GlobalLoggerBuilder::build() {
    if (logger_name_ == nullptr) {
        return {};
    }
    if (formatter_.get() == nullptr) {
        formatter_ = std::make_shared<Formatter>();
    }
    if (sinks_.empty()) {
        build_logger_sink<StdOutSink>();
    }
    Logger::ptr logger;
    if (logger_type_ == LoggerType::LOGGER_ASYNC) {
        logger = std::make_shared<AsyncLogger>(logger_name_, limit_level_,
                                               formatter_, sinks_, looper_type_,
                                               milliseco_);
    } else {
        logger = std::make_shared<SyncLogger>(logger_name_, limit_level_,
                                              formatter_, sinks_);
    }
    LoggerManager::get_instance().add_logger(logger);
    return logger;
}

} // namespace zlog
