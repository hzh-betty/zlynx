#ifndef ZNET_LOGGER_H_
#define ZNET_LOGGER_H_

#include "zlog.h"

namespace znet {

/**
 * @brief 初始化znet专属日志器（同时初始化zcoroutine日志器）
 * @param level 日志级别，默认为DEBUG
 * @note 该函数会同时初始化 znet 和 zcoroutine 两个日志系统
 */
void init_logger(zlog::LogLevel::value level = zlog::LogLevel::value::DEBUG);
zlog::Logger *get_logger();
} // namespace znet

// 便利的日志宏定义
#define ZNET_LOG_DEBUG(fmt, ...)                                               \
  znet::get_logger()->ZLOG_DEBUG(fmt, ##__VA_ARGS__)
#define ZNET_LOG_INFO(fmt, ...)                                                \
  znet::get_logger()->ZLOG_INFO(fmt, ##__VA_ARGS__)
#define ZNET_LOG_WARN(fmt, ...)                                                \
  znet::get_logger()->ZLOG_WARN(fmt, ##__VA_ARGS__)
#define ZNET_LOG_ERROR(fmt, ...)                                               \
  znet::get_logger()->ZLOG_ERROR(fmt, ##__VA_ARGS__)
#define ZNET_LOG_FATAL(fmt, ...)                                               \
  znet::get_logger()->ZLOG_FATAL(fmt, ##__VA_ARGS__)

#endif // ZCOROUTINE_LOGGER_H_
