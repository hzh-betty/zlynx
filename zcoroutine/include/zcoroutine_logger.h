#ifndef ZCOROUTINE_LOGGER_H_
#define ZCOROUTINE_LOGGER_H_

#include <string>
#include "zlog.h"

namespace zcoroutine {

// 全局日志器对象
inline zlog::Logger::ptr zcoroutine_logger;

/**
 * @brief 初始化zcoroutine专属日志器
 * @param level 日志级别，默认为DEBUG
 */
inline void InitLogger(const zlog::LogLevel::value level = zlog::LogLevel::value::DEBUG) {
    const std::unique_ptr<zlog::GlobalLoggerBuilder> builder(new zlog::GlobalLoggerBuilder());
    builder->buildLoggerName("zcoroutine_logger");
    builder->buildLoggerLevel(level);
    // 日志格式：[文件:行号] [时间戳] 日志内容
    builder->buildLoggerFormatter("[%f:%l] [%d{%Y-%m-%d %H:%M:%S}] %m%n");
    builder->buildLoggerType(zlog::LoggerType::LOGGER_ASYNC);
    builder->buildLoggerSink<zlog::FileSink>("./logfile/zcoroutine.log");
    builder->buildLoggerSink<zlog::StdOutSink>();
    zcoroutine_logger = builder->build();
}

} // namespace zcoroutine

// 便利的日志宏定义
#define ZCOROUTINE_LOG_DEBUG(fmt, ...) zcoroutine::zcoroutine_logger->ZLOG_DEBUG(fmt, ##__VA_ARGS__)
#define ZCOROUTINE_LOG_INFO(fmt, ...)  zcoroutine::zcoroutine_logger->ZLOG_INFO(fmt, ##__VA_ARGS__)
#define ZCOROUTINE_LOG_WARN(fmt, ...)  zcoroutine::zcoroutine_logger->ZLOG_WARN(fmt, ##__VA_ARGS__)
#define ZCOROUTINE_LOG_ERROR(fmt, ...) zcoroutine::zcoroutine_logger->ZLOG_ERROR(fmt, ##__VA_ARGS__)
#define ZCOROUTINE_LOG_FATAL(fmt, ...) zcoroutine::zcoroutine_logger->ZLOG_FATAL(fmt, ##__VA_ARGS__)

#endif // ZCOROUTINE_LOGGER_H_
