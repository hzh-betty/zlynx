#include "znet/znet_logger.h"

namespace znet {

namespace {

constexpr char kLoggerName[] = "znet_logger";
constexpr char kModuleName[] = "znet";

zlog::Logger::ptr& cached_logger() {
  static zlog::Logger::ptr logger;
  return logger;
}

}  // namespace

void configure_logger(const zlog::LoggerConfig& config) {
  zlog::set_module_logger_config(kModuleName, config);
  cached_logger() = zlog::rebuild_module_logger(kLoggerName, kModuleName);
}

void init_logger(const zlog::LogLevel::value level) {
  zlog::LoggerConfig config = zlog::resolve_logger_config(kModuleName);
  config.level = level;
  if (config.file_path.empty()) {
    config.file_path = "./logfile/znet.log";
  }
  configure_logger(config);
}

zlog::Logger* get_logger() {
  auto& logger = cached_logger();
  if (!logger) {
    logger = zlog::ensure_module_logger(kLoggerName, kModuleName);
  }
  if (!logger) {
    init_logger(zlog::LogLevel::value::INFO);
    logger = zlog::ensure_module_logger(kLoggerName, kModuleName);
  }
  return logger.get();
}

}  // namespace znet