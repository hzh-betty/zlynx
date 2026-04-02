#ifndef ZLOG_LOGGING_CONFIG_H_
#define ZLOG_LOGGING_CONFIG_H_

#include "level.h"
#include "logger.h"

#include <string>
#include <unordered_map>

namespace zlog {

enum class LogSinkMode {
  kStdout = 0,
  kFile = 1,
  kStdoutAndFile = 2,
};

struct LoggerConfig {
  LogLevel::value level = LogLevel::value::INFO;
  std::string formatter = "[%d{%Y-%m-%d %H:%M:%S}][%c][%p][%t] %m%n";
  LogSinkMode sink_mode = LogSinkMode::kStdout;
  std::string file_path;
  LoggerType logger_type = LoggerType::LOGGER_SYNC;
};

struct LoggingConfig {
  LoggerConfig default_config;
  std::unordered_map<std::string, LoggerConfig> module_configs;
};

void set_logging_config(const LoggingConfig& config);

LoggingConfig get_logging_config();

void set_default_logger_config(const LoggerConfig& config);

void set_module_logger_config(const std::string& module,
                              const LoggerConfig& config);

LoggerConfig resolve_logger_config(const std::string& module);

Logger::ptr ensure_module_logger(const char* logger_name, const char* module);

Logger::ptr rebuild_module_logger(const char* logger_name, const char* module);

}  // namespace zlog

#endif  // ZLOG_LOGGING_CONFIG_H_
