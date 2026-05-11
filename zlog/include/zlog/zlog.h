#ifndef ZLOG_ZLOG_H_
#define ZLOG_ZLOG_H_
#include <stdexcept>

#include "zlog/logger.h"

namespace zlog {
// 1. 提供获取指定日志器的全局接口--避免用户使用单例对象创建
/**
 * @brief 获取指定名称的日志器
 * @param name 日志器名称
 * @return 返回对应名称的日志器智能指针，如果不存在则返回空指针
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

#endif // ZLOG_ZLOG_H_
