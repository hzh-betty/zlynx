#ifndef ZLOG_ZLOG_H_
#define ZLOG_ZLOG_H_
#include "logger.h"
namespace zlog {
// 1. 提供获取指定日志器的全局接口--避免用户使用单例对象创建
/**
 * @brief 获取指定名称的日志器
 * @param name 日志器名称
 * @return 返回对应名称的日志器智能指针，如果不存在则返回空指针
 */
inline Logger::ptr getLogger(const std::string &name) {
  return LoggerManager::getInstance().getLogger(name);
}
/**
 * @brief 获取root日志器
 * @return 返回root日志器的智能指针
 */
inline Logger::ptr rootLogger() {
  return LoggerManager::getInstance().rootLogger();
}

// 2. 通过宏函数对日志器的接口进行代理
#define ZLOG_DEBUG(fmt, ...) debug(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define ZLOG_INFO(fmt, ...) info(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define ZLOG_WARN(fmt, ...) warning(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define ZLOG_ERROR(fmt, ...) error(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define ZLOG_FATAL(fmt, ...) fatal(__FILE__, __LINE__, fmt, ##__VA_ARGS__)

// 3. 提供宏函数，直接通过默认日志器打印
#define DEBUG(fmt, ...) zlog::rootLogger()->ZLOG_DEBUG(fmt, ##__VA_ARGS__)
#define INFO(fmt, ...) zlog::rootLogger()->ZLOG_INFO(fmt, ##__VA_ARGS__)
#define WARN(fmt, ...) zlog::rootLogger()->ZLOG_WARN(fmt, ##__VA_ARGS__)
#define ERROR(fmt, ...) zlog::rootLogger()->ZLOG_ERROR(fmt, ##__VA_ARGS__)
#define FATAL(fmt, ...) zlog::rootLogger()->ZLOG_FATAL(fmt, ##__VA_ARGS__)

} // namespace zlog

#endif // ZLOG_ZLOG_H_
