#include "znet_logger.h"
#include "util/zcoroutine_logger.h"

namespace znet {
void init_logger(const zlog::LogLevel::value level) {
  // 1. 初始化 zcoroutine 日志系统
  zcoroutine::init_logger(level);

  // 2. 初始化 znet 日志系统
  auto builder = std::make_unique<zlog::GlobalLoggerBuilder>();
  builder->buildLoggerName("znet_logger");
  builder->buildLoggerLevel(level);
  // 日志格式：日志等级 [文件:行号] [时间戳] 日志内容
  builder->buildLoggerFormatter("[%p] [%f:%l] [%d{%Y-%m-%d %H:%M:%S}][%t] %m%n");
  builder->buildLoggerType(zlog::LoggerType::LOGGER_SYNC);
  builder->buildLoggerSink<zlog::FileSink>("./logfile/znet.log");
  builder->buildLoggerSink<zlog::StdOutSink>();
  builder->build();
}
zlog::Logger *get_logger() {
  static zlog::Logger::ptr logger = zlog::getLogger("znet_logger");
  return logger.get();
}
} // namespace znet