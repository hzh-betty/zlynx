#ifndef ZCO_LOG_H_
#define ZCO_LOG_H_

#include "zlog/logger.h"

namespace zco {

constexpr const char *kLoggerName = "zco_logger";
constexpr const char *kDefaultFormatter = "[%d{%H:%M:%S}][%c][%p]%T%m%n";

inline void
init_logger(zlog::LogLevel::value level = zlog::LogLevel::value::INFO) {
    zlog::GlobalLoggerBuilder builder;
    builder.build_logger_name(kLoggerName);
    builder.build_logger_level(level);
    builder.build_logger_type(zlog::LoggerType::LOGGER_ASYNC);
    builder.build_logger_formatter(kDefaultFormatter);
    builder.build_logger_sink<zlog::StdOutSink>();

    zlog::Logger::ptr logger = builder.build();
    zlog::LoggerManager::get_instance().upsert_logger(kLoggerName, logger);
}

inline zlog::Logger::ptr get_logger_ptr() {
    zlog::Logger::ptr logger =
        zlog::LoggerManager::get_instance().get_logger(kLoggerName);
    if (!logger) {
        init_logger(zlog::LogLevel::value::INFO);
        logger = zlog::LoggerManager::get_instance().get_logger(kLoggerName);
    }
    return logger;
}

} // namespace zco

/**
 * @brief DEBUG 级别日志宏。
 */
#define ZCO_LOG_DEBUG(...)                                                     \
    do {                                                                       \
        auto zco_logger__ = ::zco::get_logger_ptr();                           \
        if (zco_logger__) {                                                    \
            zco_logger__->debug(__FILE__, __LINE__, __VA_ARGS__);              \
        }                                                                      \
    } while (0)

/**
 * @brief INFO 级别日志宏。
 */
#define ZCO_LOG_INFO(...)                                                      \
    do {                                                                       \
        auto zco_logger__ = ::zco::get_logger_ptr();                           \
        if (zco_logger__) {                                                    \
            zco_logger__->info(__FILE__, __LINE__, __VA_ARGS__);               \
        }                                                                      \
    } while (0)

/**
 * @brief WARN 级别日志宏。
 */
#define ZCO_LOG_WARN(...)                                                      \
    do {                                                                       \
        auto zco_logger__ = ::zco::get_logger_ptr();                           \
        if (zco_logger__) {                                                    \
            zco_logger__->warning(__FILE__, __LINE__, __VA_ARGS__);            \
        }                                                                      \
    } while (0)

/**
 * @brief ERROR 级别日志宏。
 */
#define ZCO_LOG_ERROR(...)                                                     \
    do {                                                                       \
        auto zco_logger__ = ::zco::get_logger_ptr();                           \
        if (zco_logger__) {                                                    \
            zco_logger__->error(__FILE__, __LINE__, __VA_ARGS__);              \
        }                                                                      \
    } while (0)

/**
 * @brief FATAL 级别日志宏。
 */
#define ZCO_LOG_FATAL(...)                                                     \
    do {                                                                       \
        auto zco_logger__ = ::zco::get_logger_ptr();                           \
        if (zco_logger__) {                                                    \
            zco_logger__->fatal(__FILE__, __LINE__, __VA_ARGS__);              \
        }                                                                      \
    } while (0)

#endif // ZCO_LOG_H_
