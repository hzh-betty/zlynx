#include "zhttp_logger.h"

#include "logger.h"
#include "sink.h"

namespace zhttp {

namespace {
// 静态日志器指针，延迟初始化
zlog::Logger::ptr g_logger;
} // namespace

void init_logger(zlog::LogLevel::value level) {
  // 构建 zhttp 专属日志器
  zlog::LocalLoggerBuilder builder;
  builder.buildLoggerName("zhttp");
  builder.buildLoggerLevel(level);
  builder.buildLoggerType(zlog::LoggerType::LOGGER_SYNC);
  builder.buildLoggerFormatter("[%d{%H:%M:%S}][%c][%p]%T%m%n");
  builder.buildLoggerSink<zlog::StdOutSink>();

  g_logger = builder.build();
}

zlog::Logger *get_logger() {
  // 如果未初始化，使用默认配置初始化
  if (!g_logger) {
    init_logger();
  }
  return g_logger.get();
}

} // namespace zhttp
