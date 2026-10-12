/**
 * @file zlog.h
 * @author hzh-betty
 */
#ifndef ZLOG_ZLOG_H_
#define ZLOG_ZLOG_H_
#include "zlog/logger_builder.h"
#include "zlog/logger_registry.h"
#include <stdexcept>

#include "zlog/logger.h"

namespace zlog {
/**
 * @brief 获取已注册的指定名称日志器。
 * @return 共享持有对应日志器的智能指针。
 * @throws std::runtime_error 指定名称尚未注册。
 */
inline Logger::ptr get_logger(const std::string &name) {
    Logger::ptr logger = LoggerManager::get_instance().get_logger(name);
    if (!logger) {
        throw std::runtime_error("logger is not initialized: " + name);
    }
    return logger;
}

/**
 * @brief 获取root日志器
 * @return 返回root日志器的智能指针
 */
inline Logger::ptr root_logger() {
    Logger::ptr logger = LoggerManager::get_instance().root_logger();
    if (!logger) {
        throw std::runtime_error("root logger is not initialized");
    }
    return logger;
}

// 2. 通过宏函数对日志器的接口进行代理
#define ZLOG_DEBUG(...) debug(__FILE__, __LINE__, __VA_ARGS__)
#define ZLOG_INFO(...) info(__FILE__, __LINE__, __VA_ARGS__)
#define ZLOG_WARN(...) warning(__FILE__, __LINE__, __VA_ARGS__)
#define ZLOG_ERROR(...) error(__FILE__, __LINE__, __VA_ARGS__)
#define ZLOG_FATAL(...) fatal(__FILE__, __LINE__, __VA_ARGS__)

// 3. 提供宏函数，直接通过默认日志器打印
#define DEBUG(...) zlog::root_logger()->ZLOG_DEBUG(__VA_ARGS__)
#define INFO(...) zlog::root_logger()->ZLOG_INFO(__VA_ARGS__)
#define WARN(...) zlog::root_logger()->ZLOG_WARN(__VA_ARGS__)
#define ERROR(...) zlog::root_logger()->ZLOG_ERROR(__VA_ARGS__)
#define FATAL(...) zlog::root_logger()->ZLOG_FATAL(__VA_ARGS__)

} // namespace zlog

#endif
