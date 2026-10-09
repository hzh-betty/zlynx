/**
 * @file logger_builder.cc
 * @brief 收集配置并构建日志器。
 */

#include "zlog/logger_builder.h"
#include "zlog/logger_registry.h"

namespace zlog {

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
    if (logger_name) {
        build_logger_name(fmt::string_view(logger_name));
    } else {
        logger_name_.clear();
        has_logger_name_ = false;
    }
}

void LoggerBuilder::build_logger_name(fmt::string_view logger_name) {
    if (logger_name.size() == 0) {
        logger_name_.clear();
    } else {
        logger_name_.assign(logger_name.data(), logger_name.size());
    }
    has_logger_name_ = true;
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

Logger::ptr LoggerBuilder::build() {
    if (!has_logger_name_) {
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

Logger::ptr LoggerBuilder::build_global() {
    Logger::ptr logger = build();
    if (logger) {
        LoggerManager::get_instance().add_logger(logger);
    }
    return logger;
}

} // namespace zlog
