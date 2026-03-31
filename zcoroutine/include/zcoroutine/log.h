#ifndef ZCOROUTINE_LOG_H_
#define ZCOROUTINE_LOG_H_

#include "zlog.h"

namespace zcoroutine {

/**
 * @brief 获取 zcoroutine 默认日志器。
 * @return 日志器智能指针，若日志系统不可用则返回空。
 */
inline zlog::Logger::ptr default_logger() {
  return zlog::rootLogger();
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
