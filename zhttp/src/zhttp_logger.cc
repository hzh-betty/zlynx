#include "zhttp_logger.h"

#include "logger.h"
#include "sink.h"
#include "znet_logger.h"

namespace zhttp {

void init_logger(zlog::LogLevel::value level) {
  znet::init_logger(level); // 同时初始化 znet 日志系统

  // 构建 zhttp 专属日志器
  zlog::LocalLoggerBuilder builder;
  builder.buildLoggerName("zhttp_logger");
  builder.buildLoggerLevel(level);
  builder.buildLoggerType(zlog::LoggerType::LOGGER_SYNC);
  builder.buildLoggerFormatter("[%d{%H:%M:%S}][%c][%p]%T%m%n");
  builder.buildLoggerSink<zlog::StdOutSink>();
  builder.build();
}

zlog::Logger *get_logger() {
 static zlog::Logger::ptr logger = zlog::getLogger("zhttp_logger");
  return logger.get();
}

} // namespace zhttp
