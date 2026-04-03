#include "znet/znet_logger.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace znet {

namespace {

constexpr char kLoggerName[] = "znet_logger";
constexpr char kDefaultFormatter[] = "[%d{%H:%M:%S}][%c][%p]%T%m%n";
constexpr char kDefaultFilePath[] = "./logfile/znet.log";

zlog::Logger::ptr& cached_logger() {
  static zlog::Logger::ptr logger;
  return logger;
}

std::string normalize_sink(std::string sink) {
  std::transform(sink.begin(), sink.end(), sink.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return sink;
}

std::string resolve_log_file_path(const LoggerInitOptions& options) {
  if (!options.file_path.empty()) {
    return options.file_path;
  }
  return kDefaultFilePath;
}

void build_logger_sinks(zlog::GlobalLoggerBuilder& builder,
                        const LoggerInitOptions& options) {
  const std::string sink = normalize_sink(options.sink);
  if (sink == "file") {
    builder.buildLoggerSink<zlog::FileSink>(resolve_log_file_path(options));
    return;
  }
  if (sink == "both" || sink == "stdout+file" || sink == "file+stdout") {
    builder.buildLoggerSink<zlog::StdOutSink>();
    builder.buildLoggerSink<zlog::FileSink>(resolve_log_file_path(options));
    return;
  }
  builder.buildLoggerSink<zlog::StdOutSink>();
}

}  // namespace

void init_logger(const LoggerInitOptions& options) {
  zlog::GlobalLoggerBuilder builder;
  builder.buildLoggerName(kLoggerName);
  builder.buildLoggerLevel(options.level);
  builder.buildLoggerType(options.async ? zlog::LoggerType::LOGGER_ASYNC
                                        : zlog::LoggerType::LOGGER_SYNC);
  builder.buildLoggerFormatter(options.formatter.empty() ? kDefaultFormatter
                                                         : options.formatter);
  build_logger_sinks(builder, options);

  zlog::Logger::ptr logger = builder.build();
  zlog::LoggerManager::getInstance().upsertLogger(kLoggerName, logger);
  cached_logger() = std::move(logger);
}

void init_logger(zlog::LogLevel::value level) {
  LoggerInitOptions options;
  options.level = level;
  init_logger(options);
}

zlog::Logger* get_logger() {
  auto& logger = cached_logger();
  if (!logger) {
    init_logger(zlog::LogLevel::value::INFO);
  }
  return logger.get();
}

}  // namespace znet