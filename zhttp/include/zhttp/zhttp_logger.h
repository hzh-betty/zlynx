#ifndef ZHTTP_LOGGER_H_
#define ZHTTP_LOGGER_H_

#include "zlog/logger.h"

namespace zhttp {

/**
 * @brief 初始化 zhttp 专属日志器
 * @param level 日志等级；初始化 zhttp 时会同步初始化 znet 和 zco。
 */
void init_logger(zlog::LogLevel::value level = zlog::LogLevel::value::INFO);

/**
 * @brief 获取 zhttp 日志器
 * @return 日志器智能指针，若初始化失败可能返回空。
 */
zlog::Logger::ptr get_logger_ptr();

} // namespace zhttp

// 便捷日志宏，统一走 zhttp 自己的日志器实例。
#define ZHTTP_LOG_DEBUG(fmt, ...)                                              \
    do {                                                                       \
        auto zhttp_logger__ = ::zhttp::get_logger_ptr();                       \
        if (zhttp_logger__) {                                                  \
            zhttp_logger__->debug(__FILE__, __LINE__, fmt, ##__VA_ARGS__);     \
        }                                                                      \
    } while (0)
#define ZHTTP_LOG_INFO(fmt, ...)                                               \
    do {                                                                       \
        auto zhttp_logger__ = ::zhttp::get_logger_ptr();                       \
        if (zhttp_logger__) {                                                  \
            zhttp_logger__->info(__FILE__, __LINE__, fmt, ##__VA_ARGS__);      \
        }                                                                      \
    } while (0)
#define ZHTTP_LOG_WARN(fmt, ...)                                               \
    do {                                                                       \
        auto zhttp_logger__ = ::zhttp::get_logger_ptr();                       \
        if (zhttp_logger__) {                                                  \
            zhttp_logger__->warning(__FILE__, __LINE__, fmt, ##__VA_ARGS__);   \
        }                                                                      \
    } while (0)
#define ZHTTP_LOG_ERROR(fmt, ...)                                              \
    do {                                                                       \
        auto zhttp_logger__ = ::zhttp::get_logger_ptr();                       \
        if (zhttp_logger__) {                                                  \
            zhttp_logger__->error(__FILE__, __LINE__, fmt, ##__VA_ARGS__);     \
        }                                                                      \
    } while (0)
#define ZHTTP_LOG_FATAL(fmt, ...)                                              \
    do {                                                                       \
        auto zhttp_logger__ = ::zhttp::get_logger_ptr();                       \
        if (zhttp_logger__) {                                                  \
            zhttp_logger__->fatal(__FILE__, __LINE__, fmt, ##__VA_ARGS__);     \
        }                                                                      \
    } while (0)

#endif // ZHTTP_LOGGER_H_
