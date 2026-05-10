#ifndef ZCO_LOG_H_
#define ZCO_LOG_H_

#include "zlog/logger.h"

namespace zco {

constexpr const char *kLoggerName = "zco_logger";
constexpr const char *kDefaultFormatter = "[%d{%H:%M:%S}][%c][%p]%T%m%n";

void init_logger(zlog::LogLevel::value level = zlog::LogLevel::value::INFO);

zlog::Logger::ptr get_logger_ptr();

bool should_log(zlog::LogLevel::value level);

} // namespace zco

/**
 * @brief DEBUG 级别日志宏。
 */
#define ZCO_LOG_DEBUG(...)                                                     \
    do {                                                                       \
        if (::zco::should_log(::zlog::LogLevel::value::DEBUG)) {               \
            auto zco_logger__ = ::zco::get_logger_ptr();                       \
            if (zco_logger__) {                                                \
                zco_logger__->debug(__FILE__, __LINE__, __VA_ARGS__);          \
            }                                                                  \
        }                                                                      \
    } while (0)

/**
 * @brief INFO 级别日志宏。
 */
#define ZCO_LOG_INFO(...)                                                      \
    do {                                                                       \
        if (::zco::should_log(::zlog::LogLevel::value::INFO)) {                \
            auto zco_logger__ = ::zco::get_logger_ptr();                       \
            if (zco_logger__) {                                                \
                zco_logger__->info(__FILE__, __LINE__, __VA_ARGS__);           \
            }                                                                  \
        }                                                                      \
    } while (0)

/**
 * @brief WARN 级别日志宏。
 */
#define ZCO_LOG_WARN(...)                                                      \
    do {                                                                       \
        if (::zco::should_log(::zlog::LogLevel::value::WARNING)) {             \
            auto zco_logger__ = ::zco::get_logger_ptr();                       \
            if (zco_logger__) {                                                \
                zco_logger__->warning(__FILE__, __LINE__, __VA_ARGS__);        \
            }                                                                  \
        }                                                                      \
    } while (0)

/**
 * @brief ERROR 级别日志宏。
 */
#define ZCO_LOG_ERROR(...)                                                     \
    do {                                                                       \
        if (::zco::should_log(::zlog::LogLevel::value::ERROR)) {               \
            auto zco_logger__ = ::zco::get_logger_ptr();                       \
            if (zco_logger__) {                                                \
                zco_logger__->error(__FILE__, __LINE__, __VA_ARGS__);          \
            }                                                                  \
        }                                                                      \
    } while (0)

/**
 * @brief FATAL 级别日志宏。
 */
#define ZCO_LOG_FATAL(...)                                                     \
    do {                                                                       \
        if (::zco::should_log(::zlog::LogLevel::value::FATAL)) {               \
            auto zco_logger__ = ::zco::get_logger_ptr();                       \
            if (zco_logger__) {                                                \
                zco_logger__->fatal(__FILE__, __LINE__, __VA_ARGS__);          \
            }                                                                  \
        }                                                                      \
    } while (0)

#endif // ZCO_LOG_H_
