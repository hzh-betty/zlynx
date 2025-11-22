#ifndef ZLYNX_LOGGER_H_
#define ZLYNX_LOGGER_H_
#include <string>

#include "zlog.h"

namespace zlynx
{
    inline zlog::Logger::ptr zlynx_logger; // 全局日志器

    inline void Init(const zlog::LogLevel::value level = zlog::LogLevel::value::DEBUG)
    {
        std::unique_ptr<zlog::GlobalLoggerBuilder> builder(new zlog::GlobalLoggerBuilder());
        builder->buildLoggerName("zlynx_logger");
        builder->buildLoggerLevel(level);
        builder->buildLoggerFormatter("[%f:%l] [%d{%Y-%m-%d %H:%M:%S}] %m%n");
        builder->buildLoggerType(zlog::LoggerType::LOGGER_ASYNC);
        builder->buildLoggerSink<zlog::FileSink>("./logfile/zlynx.log");
        builder->buildLoggerSink<zlog::StdOutSink>();
        zlynx_logger = builder->build();
    }

} // namespace zlynx
// 便利的日志宏定义
#define ZLYNX_LOG_DEBUG(fmt, ...) zlynx::zlynx_logger->ZLOG_DEBUG(fmt, ##__VA_ARGS__)
#define ZLYNX_LOG_INFO(fmt, ...) zlynx::zlynx_logger->ZLOG_INFO(fmt, ##__VA_ARGS__)
#define ZLYNX_LOG_WARN(fmt, ...) zlynx::zlynx_logger->ZLOG_WARN(fmt, ##__VA_ARGS__)
#define ZLYNX_LOG_ERROR(fmt, ...)zlynx::zlynx_logger->ZLOG_ERROR(fmt, ##__VA_ARGS__)
#define ZLYNX_LOG_FATAL(fmt, ...) zlynx::zlynx_logger->ZLOG_FATAL(fmt, ##__VA_ARGS__)
#endif //ZLYNX_LOGGER_H_
