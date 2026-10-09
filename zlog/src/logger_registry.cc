/**
 * @file logger_registry.cc
 * @brief 保存和查询命名日志器。
 */

#include "zlog/logger_registry.h"

namespace zlog {

LoggerManager::LoggerManager() {
    // 默认全局入口直接装配同步日志器，注册表不依赖 Builder。
    const std::vector<LogSink::ptr> sinks{std::make_shared<StdOutSink>()};
    root_logger_ = std::make_shared<SyncLogger>(
        "root", LogLevel::value::DEBUG, std::make_shared<Formatter>(), sinks);
    loggers_.insert({"root", root_logger_});
}

void LoggerManager::add_logger(Logger::ptr &logger) {
    std::unique_lock<std::mutex> lock(mutex_);
    loggers_.insert({logger->get_name(), logger});
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

} // namespace zlog
