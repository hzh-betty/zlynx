#ifndef ZCOROUTINE_LOG_H_
#define ZCOROUTINE_LOG_H_

#include "zlog.h"

namespace zcoroutine {

constexpr const char* kLoggerName = "zcoroutine_logger";
constexpr const char* kModuleName = "zcoroutine";

inline void configure_logger(const zlog::LoggerConfig& config) {
  zlog::set_module_logger_config(kModuleName, config);
  (void)zlog::rebuild_module_logger(kLoggerName, kModuleName);
}

inline void init_logger(
    zlog::LogLevel::value level = zlog::LogLevel::value::INFO) {
  zlog::LoggerConfig config = zlog::resolve_logger_config(kModuleName);
  config.level = level;
  configure_logger(config);
}

/**
 * @brief 获取 zcoroutine 默认日志器。
 * @return 日志器智能指针，若日志系统不可用则返回空。
 */
inline zlog::Logger::ptr default_logger() {
  return zlog::ensure_module_logger(kLoggerName, kModuleName);
}

}  // namespace zcoroutine

/**
 * @brief DEBUG 级别日志宏。
 */
#define ZCOROUTINE_LOG_DEBUG(...)                                                     \
  do {                                                                                \
    auto zcoroutine_logger__ = ::zcoroutine::default_logger();                        \
    if (zcoroutine_logger__) {                                                        \
      zcoroutine_logger__->debug(__FILE__, __LINE__, __VA_ARGS__);                   \
    }                                                                                 \
  } while (0)

/**
 * @brief INFO 级别日志宏。
 */
#define ZCOROUTINE_LOG_INFO(...)                                                      \
  do {                                                                                \
    auto zcoroutine_logger__ = ::zcoroutine::default_logger();                        \
    if (zcoroutine_logger__) {                                                        \
      zcoroutine_logger__->info(__FILE__, __LINE__, __VA_ARGS__);                    \
    }                                                                                 \
  } while (0)

/**
 * @brief WARN 级别日志宏。
 */
#define ZCOROUTINE_LOG_WARN(...)                                                      \
  do {                                                                                \
    auto zcoroutine_logger__ = ::zcoroutine::default_logger();                        \
    if (zcoroutine_logger__) {                                                        \
      zcoroutine_logger__->warning(__FILE__, __LINE__, __VA_ARGS__);                 \
    }                                                                                 \
  } while (0)

/**
 * @brief ERROR 级别日志宏。
 */
#define ZCOROUTINE_LOG_ERROR(...)                                                     \
  do {                                                                                \
    auto zcoroutine_logger__ = ::zcoroutine::default_logger();                        \
    if (zcoroutine_logger__) {                                                        \
      zcoroutine_logger__->error(__FILE__, __LINE__, __VA_ARGS__);                   \
    }                                                                                 \
  } while (0)

/**
 * @brief FATAL 级别日志宏。
 */
#define ZCOROUTINE_LOG_FATAL(...)                                                     \
  do {                                                                                \
    auto zcoroutine_logger__ = ::zcoroutine::default_logger();                        \
    if (zcoroutine_logger__) {                                                        \
      zcoroutine_logger__->fatal(__FILE__, __LINE__, __VA_ARGS__);                   \
    }                                                                                 \
  } while (0)

#endif  // ZCOROUTINE_LOG_H_
