#ifndef ZCOROUTINE_LOG_H_
#define ZCOROUTINE_LOG_H_

#include "logger.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace zcoroutine {

constexpr const char* kLoggerName = "zcoroutine_logger";
constexpr const char* kDefaultFormatter = "[%d{%H:%M:%S}][%c][%p]%T%m%n";
constexpr const char* kDefaultFilePath = "./logfile/zcoroutine.log";

struct LoggerInitOptions {
  zlog::LogLevel::value level = zlog::LogLevel::value::DEBUG;
  bool async = true;
  std::string formatter = kDefaultFormatter;
  std::string sink = "stdout";
  std::string file_path;
};

inline std::string normalize_sink(std::string sink) {
  std::transform(sink.begin(), sink.end(), sink.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return sink;
}

inline void build_sinks(zlog::GlobalLoggerBuilder& builder,
                        const LoggerInitOptions& options) {
  const std::string sink = normalize_sink(options.sink);
  const std::string path =
      options.file_path.empty() ? std::string{kDefaultFilePath} : options.file_path;

  if (sink == "file") {
    builder.buildLoggerSink<zlog::FileSink>(path);
    return;
  }

  if (sink == "both" || sink == "stdout+file" || sink == "file+stdout") {
    builder.buildLoggerSink<zlog::StdOutSink>();
    builder.buildLoggerSink<zlog::FileSink>(path);
    return;
  }

  builder.buildLoggerSink<zlog::StdOutSink>();
}

inline zlog::Logger::ptr& cached_logger() {
  static zlog::Logger::ptr logger;
  return logger;
}

inline void init_logger(const LoggerInitOptions& options) {
  zlog::GlobalLoggerBuilder builder;
  builder.buildLoggerName(kLoggerName);
  builder.buildLoggerLevel(options.level);
  builder.buildLoggerType(options.async ? zlog::LoggerType::LOGGER_ASYNC
                                        : zlog::LoggerType::LOGGER_SYNC);
  builder.buildLoggerFormatter(options.formatter.empty() ? kDefaultFormatter
                                                         : options.formatter);
  build_sinks(builder, options);

  zlog::Logger::ptr logger = builder.build();
  zlog::LoggerManager::getInstance().upsertLogger(kLoggerName, logger);
  cached_logger() = std::move(logger);
}

inline void init_logger(
    zlog::LogLevel::value level = zlog::LogLevel::value::INFO) {
  LoggerInitOptions options;
  options.level = level;
  init_logger(options);
}

inline zlog::Logger* get_logger() {
  auto& logger = cached_logger();
  if (!logger) {
    init_logger(zlog::LogLevel::value::INFO);
  }
  return logger.get();
}

/**
 * @brief 获取 zcoroutine 默认日志器。
 * @return 日志器智能指针，若日志系统不可用则返回空。
 */
inline zlog::Logger::ptr default_logger() {
  auto& logger = cached_logger();
  if (!logger) {
    init_logger(zlog::LogLevel::value::INFO);
  }
  return logger;
}

}  // namespace zcoroutine

/**
 * @brief DEBUG 级别日志宏。
 */
#if defined(ZCOROUTINE_ENABLE_DEBUG_LOGS)
#define ZCOROUTINE_LOG_DEBUG(...)                                                     \
  do {                                                                                \
    auto* zcoroutine_logger__ = ::zcoroutine::get_logger();                           \
    if (zcoroutine_logger__) {                                                        \
      zcoroutine_logger__->debug(__FILE__, __LINE__, __VA_ARGS__);                   \
    }                                                                                 \
  } while (0)
#else
#define ZCOROUTINE_LOG_DEBUG(...)                                                     \
  do {                                                                                \
  } while (0)
#endif

/**
 * @brief INFO 级别日志宏。
 */
#define ZCOROUTINE_LOG_INFO(...)                                                      \
  do {                                                                                \
    auto* zcoroutine_logger__ = ::zcoroutine::get_logger();                           \
    if (zcoroutine_logger__) {                                                        \
      zcoroutine_logger__->info(__FILE__, __LINE__, __VA_ARGS__);                    \
    }                                                                                 \
  } while (0)

/**
 * @brief WARN 级别日志宏。
 */
#define ZCOROUTINE_LOG_WARN(...)                                                      \
  do {                                                                                \
    auto* zcoroutine_logger__ = ::zcoroutine::get_logger();                           \
    if (zcoroutine_logger__) {                                                        \
      zcoroutine_logger__->warning(__FILE__, __LINE__, __VA_ARGS__);                 \
    }                                                                                 \
  } while (0)

/**
 * @brief ERROR 级别日志宏。
 */
#define ZCOROUTINE_LOG_ERROR(...)                                                     \
  do {                                                                                \
    auto* zcoroutine_logger__ = ::zcoroutine::get_logger();                           \
    if (zcoroutine_logger__) {                                                        \
      zcoroutine_logger__->error(__FILE__, __LINE__, __VA_ARGS__);                   \
    }                                                                                 \
  } while (0)

/**
 * @brief FATAL 级别日志宏。
 */
#define ZCOROUTINE_LOG_FATAL(...)                                                     \
  do {                                                                                \
    auto* zcoroutine_logger__ = ::zcoroutine::get_logger();                           \
    if (zcoroutine_logger__) {                                                        \
      zcoroutine_logger__->fatal(__FILE__, __LINE__, __VA_ARGS__);                   \
    }                                                                                 \
  } while (0)

#endif  // ZCOROUTINE_LOG_H_
