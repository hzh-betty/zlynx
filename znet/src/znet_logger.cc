#include "znet/znet_logger.h"

#include <memory>

namespace znet {
// 构建并注册 znet 默认日志器，输出到文件与标准输出双通道。
void init_logger(const zlog::LogLevel::value level) {
  // 初始化 znet 日志系统
  auto builder = std::make_unique<zlog::GlobalLoggerBuilder>();
  builder->buildLoggerName("znet_logger");
  builder->buildLoggerLevel(level);
  // 日志格式：日志等级 [文件:行号] [时间戳] 日志内容
  builder->buildLoggerFormatter(
      "[%p] [%f:%l] [%d{%Y-%m-%d %H:%M:%S}][%t] %m%n");
  builder->buildLoggerType(zlog::LoggerType::LOGGER_SYNC);
  builder->buildLoggerSink<zlog::FileSink>("./logfile/znet.log");
  builder->buildLoggerSink<zlog::StdOutSink>();
  builder->build();
}

// 惰性获取日志器：若未初始化则按默认配置补建一次。
zlog::Logger *get_logger() {
  static zlog::Logger::ptr logger;
  if (!logger) {
    // 优先尝试获取已有注册实例，避免重复构建。
    logger = zlog::getLogger("znet_logger");
  }
  if (!logger) {
    // 首次使用且未注册时，使用默认 DEBUG 级别初始化。
    init_logger(zlog::LogLevel::value::DEBUG);
    logger = zlog::getLogger("znet_logger");
  }
  return logger.get();
}
} // namespace znet