#include "logging_config.h"

#include "sink.h"

#include <mutex>
#include <utility>

namespace zlog {
namespace {

std::mutex g_config_mutex;
LoggingConfig g_logging_config;

LoggerConfig normalize_config(LoggerConfig config, const std::string& module) {
  if (config.formatter.empty()) {
    config.formatter = "[%d{%Y-%m-%d %H:%M:%S}][%c][%p][%t] %m%n";
  }

  if ((config.sink_mode == LogSinkMode::kFile ||
       config.sink_mode == LogSinkMode::kStdoutAndFile) &&
      config.file_path.empty()) {
    config.file_path = "./logfile/" + module + ".log";
  }

  return config;
}

Logger::ptr build_logger(const char* logger_name, const LoggerConfig& config) {
  LocalLoggerBuilder builder;
  builder.buildLoggerName(logger_name);
  builder.buildLoggerLevel(config.level);
  builder.buildLoggerType(config.logger_type);
  builder.buildLoggerFormatter(config.formatter);

  switch (config.sink_mode) {
    case LogSinkMode::kStdout:
      builder.buildLoggerSink<StdOutSink>();
      break;
    case LogSinkMode::kFile:
      builder.buildLoggerSink<FileSink>(config.file_path);
      break;
    case LogSinkMode::kStdoutAndFile:
      builder.buildLoggerSink<FileSink>(config.file_path);
      builder.buildLoggerSink<StdOutSink>();
      break;
  }

  return builder.build();
}

}  // namespace

void set_logging_config(const LoggingConfig& config) {
  std::lock_guard<std::mutex> lock(g_config_mutex);
  g_logging_config = config;
}

LoggingConfig get_logging_config() {
  std::lock_guard<std::mutex> lock(g_config_mutex);
  return g_logging_config;
}

void set_default_logger_config(const LoggerConfig& config) {
  std::lock_guard<std::mutex> lock(g_config_mutex);
  g_logging_config.default_config = config;
}

void set_module_logger_config(const std::string& module,
                              const LoggerConfig& config) {
  std::lock_guard<std::mutex> lock(g_config_mutex);
  g_logging_config.module_configs[module] = config;
}

LoggerConfig resolve_logger_config(const std::string& module) {
  std::lock_guard<std::mutex> lock(g_config_mutex);

  auto config = g_logging_config.default_config;
  const auto iter = g_logging_config.module_configs.find(module);
  if (iter != g_logging_config.module_configs.end()) {
    config = iter->second;
  }

  return normalize_config(std::move(config), module);
}

Logger::ptr ensure_module_logger(const char* logger_name, const char* module) {
  auto logger = LoggerManager::getInstance().getLogger(logger_name);
  if (logger) {
    return logger;
  }

  return rebuild_module_logger(logger_name, module);
}

Logger::ptr rebuild_module_logger(const char* logger_name, const char* module) {
  const LoggerConfig config = resolve_logger_config(module);
  Logger::ptr logger = build_logger(logger_name, config);
  if (!logger) {
    return logger;
  }

  LoggerManager::getInstance().upsertLogger(logger_name, logger);
  return logger;
}

}  // namespace zlog
