#ifndef ZHTTP_LOGGER_H_
#define ZHTTP_LOGGER_H_

#include "zlog.h"

namespace zhttp {

/**
 * @brief 初始化 zhttp 专属日志器
 * @param level 日志级别，默认为 DEBUG
 */
void init_logger(zlog::LogLevel::value level = zlog::LogLevel::value::DEBUG);

/**
 * @brief 获取 zhttp 日志器
 * @return 日志器指针
 */
zlog::Logger *get_logger();

} // namespace zhttp

// 便利的日志宏定义
#define ZHTTP_LOG_DEBUG(fmt, ...)                                              \
  zhttp::get_logger()->ZLOG_DEBUG(fmt, ##__VA_ARGS__)
#define ZHTTP_LOG_INFO(fmt, ...)                                               \
  zhttp::get_logger()->ZLOG_INFO(fmt, ##__VA_ARGS__)
#define ZHTTP_LOG_WARN(fmt, ...)                                               \
  zhttp::get_logger()->ZLOG_WARN(fmt, ##__VA_ARGS__)
#define ZHTTP_LOG_ERROR(fmt, ...)                                              \
  zhttp::get_logger()->ZLOG_ERROR(fmt, ##__VA_ARGS__)
#define ZHTTP_LOG_FATAL(fmt, ...)                                              \
  zhttp::get_logger()->ZLOG_FATAL(fmt, ##__VA_ARGS__)

#endif // ZHTTP_LOGGER_H_
