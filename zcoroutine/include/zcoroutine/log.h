#ifndef ZCOROUTINE_LOG_H_
#define ZCOROUTINE_LOG_H_

#include "logger.h"

#include <algorithm>
#include <atomic>
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
    builder.build_logger_sink<zlog::FileSink>(path);
    return;
  }

  if (sink == "both" || sink == "stdout+file" || sink == "file+stdout") {
    builder.build_logger_sink<zlog::StdOutSink>();
    builder.build_logger_sink<zlog::FileSink>(path);
    return;
  }

  builder.build_logger_sink<zlog::StdOutSink>();
}

inline zlog::Logger::ptr& cached_logger() {
  static zlog::Logger::ptr logger;
  return logger;
}

inline std::atomic<int>& cached_logger_level() {
  static std::atomic<int> level(static_cast<int>(zlog::LogLevel::value::INFO));
  return level;
}

inline void init_logger(const LoggerInitOptions& options) {
  zlog::GlobalLoggerBuilder builder;
  builder.build_logger_name(kLoggerName);
  builder.build_logger_level(options.level);
  builder.build_logger_type(options.async ? zlog::LoggerType::LOGGER_ASYNC
                                        : zlog::LoggerType::LOGGER_SYNC);
  builder.build_logger_formatter(options.formatter.empty() ? kDefaultFormatter
                                                         : options.formatter);
  build_sinks(builder, options);

  zlog::Logger::ptr logger = builder.build();
  zlog::LoggerManager::get_instance().upsert_logger(kLoggerName, logger);
  cached_logger() = std::move(logger);
  cached_logger_level().store(static_cast<int>(options.level), std::memory_order_release);
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

inline bool should_log(zlog::LogLevel::value level) {
  return static_cast<int>(level) >= cached_logger_level().load(std::memory_order_acquire);
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
#define ZCOROUTINE_LOG_DEBUG(...)                                                     \
  do {                                                                                \
    if (::zcoroutine::should_log(::zlog::LogLevel::value::DEBUG)) {                  \
      auto* zcoroutine_logger__ = ::zcoroutine::get_logger();                         \
      if (zcoroutine_logger__) {                                                      \
        zcoroutine_logger__->debug(__FILE__, __LINE__, __VA_ARGS__);                 \
      }                                                                               \
    }                                                                                 \
  } while (0)
  
/**
 * @brief INFO 级别日志宏。
 */
#define ZCOROUTINE_LOG_INFO(...)                                                      \
  do {                                                                                \
    if (::zcoroutine::should_log(::zlog::LogLevel::value::INFO)) {                   \
      auto* zcoroutine_logger__ = ::zcoroutine::get_logger();                         \
      if (zcoroutine_logger__) {                                                      \
        zcoroutine_logger__->info(__FILE__, __LINE__, __VA_ARGS__);                  \
      }                                                                               \
    }                                                                                 \
  } while (0)

/**
 * @brief WARN 级别日志宏。
 */
#define ZCOROUTINE_LOG_WARN(...)                                                      \
  do {                                                                                \
    if (::zcoroutine::should_log(::zlog::LogLevel::value::WARNING)) {                \
      auto* zcoroutine_logger__ = ::zcoroutine::get_logger();                         \
      if (zcoroutine_logger__) {                                                      \
        zcoroutine_logger__->warning(__FILE__, __LINE__, __VA_ARGS__);               \
      }                                                                               \
    }                                                                                 \
  } while (0)

/**
 * @brief ERROR 级别日志宏。
 */
#define ZCOROUTINE_LOG_ERROR(...)                                                     \
  do {                                                                                \
    if (::zcoroutine::should_log(::zlog::LogLevel::value::ERROR)) {                  \
      auto* zcoroutine_logger__ = ::zcoroutine::get_logger();                         \
      if (zcoroutine_logger__) {                                                      \
        zcoroutine_logger__->error(__FILE__, __LINE__, __VA_ARGS__);                 \
      }                                                                               \
    }                                                                                 \
  } while (0)

/**
 * @brief FATAL 级别日志宏。
 */
#define ZCOROUTINE_LOG_FATAL(...)                                                     \
  do {                                                                                \
    if (::zcoroutine::should_log(::zlog::LogLevel::value::FATAL)) {                  \
      auto* zcoroutine_logger__ = ::zcoroutine::get_logger();                         \
      if (zcoroutine_logger__) {                                                      \
        zcoroutine_logger__->fatal(__FILE__, __LINE__, __VA_ARGS__);                 \
      }                                                                               \
    }                                                                                 \
  } while (0)

#endif  // ZCOROUTINE_LOG_H_
