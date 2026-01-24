#include "zcoroutine_logger.h"

namespace zcoroutine {
void init_logger(const zlog::LogLevel::value level) {
  auto builder = std::make_unique<zlog::GlobalLoggerBuilder>();
  builder->buildLoggerName("zcoroutine_logger");
  builder->buildLoggerLevel(level);
  // 日志格式：日志等级 [文件:行号] [时间戳] 日志内容
  builder->buildLoggerFormatter(
      "[%p] [%f:%l] [%d{%Y-%m-%d %H:%M:%S}][%t] %m%n");
  builder->buildLoggerType(zlog::LoggerType::LOGGER_SYNC);
  builder->buildLoggerSink<zlog::FileSink>("./logfile/zcoroutine.log");
  builder->buildLoggerSink<zlog::StdOutSink>();
  builder->build();
}
zlog::Logger::ptr get_logger() {
  static zlog::Logger::ptr logger = zlog::getLogger("zcoroutine_logger");
  return logger;
}
} // namespace zcoroutine