/**
 * zhttp_logger.h
 * zhttp_logger 定义。
 *
 * @author hzh-betty
 */

#ifndef ZHTTP_LOGGER_H_
#define ZHTTP_LOGGER_H_

#include "zlog/logger.h"

namespace zhttp {

/**
 * 初始化 zhttp 专属日志器
 *
 * @param level 日志等级；初始化 zhttp 时会同步初始化 znet 和 zco。
 */
void init_logger(zlog::LogLevel::value level = zlog::LogLevel::value::INFO);

/**
 * 获取 zhttp 日志器
 *
 * @return 日志器智能指针，若初始化失败可能返回空。
 */
zlog::Logger::ptr get_logger_ptr();

/**
 * 检查当前日志器是否启用指定等级，供日志宏在格式化参数前判断。
 *
 * @param level 待输出的日志等级。
 * @return 日志器允许该等级时返回 true。
 */
bool should_log(zlog::LogLevel::value level);

} // namespace zhttp

// 便捷日志宏，统一走 zhttp 自己的日志器实例。
#define ZHTTP_LOG_DEBUG(...)                                                   \
    do {                                                                       \
        if (::zhttp::should_log(::zlog::LogLevel::value::DEBUG)) {             \
            auto zhttp_logger__ = ::zhttp::get_logger_ptr();                   \
            if (zhttp_logger__) {                                              \
                zhttp_logger__->debug(__FILE__, __LINE__, __VA_ARGS__);        \
            }                                                                  \
        }                                                                      \
    } while (0)
#define ZHTTP_LOG_INFO(...)                                                    \
    do {                                                                       \
        if (::zhttp::should_log(::zlog::LogLevel::value::INFO)) {              \
            auto zhttp_logger__ = ::zhttp::get_logger_ptr();                   \
            if (zhttp_logger__) {                                              \
                zhttp_logger__->info(__FILE__, __LINE__, __VA_ARGS__);         \
            }                                                                  \
        }                                                                      \
    } while (0)
#define ZHTTP_LOG_WARN(...)                                                    \
    do {                                                                       \
        if (::zhttp::should_log(::zlog::LogLevel::value::WARNING)) {           \
            auto zhttp_logger__ = ::zhttp::get_logger_ptr();                   \
            if (zhttp_logger__) {                                              \
                zhttp_logger__->warning(__FILE__, __LINE__, __VA_ARGS__);      \
            }                                                                  \
        }                                                                      \
    } while (0)
#define ZHTTP_LOG_ERROR(...)                                                   \
    do {                                                                       \
        if (::zhttp::should_log(::zlog::LogLevel::value::ERROR)) {             \
            auto zhttp_logger__ = ::zhttp::get_logger_ptr();                   \
            if (zhttp_logger__) {                                              \
                zhttp_logger__->error(__FILE__, __LINE__, __VA_ARGS__);        \
            }                                                                  \
        }                                                                      \
    } while (0)
#define ZHTTP_LOG_FATAL(...)                                                   \
    do {                                                                       \
        if (::zhttp::should_log(::zlog::LogLevel::value::FATAL)) {             \
            auto zhttp_logger__ = ::zhttp::get_logger_ptr();                   \
            if (zhttp_logger__) {                                              \
                zhttp_logger__->fatal(__FILE__, __LINE__, __VA_ARGS__);        \
            }                                                                  \
        }                                                                      \
    } while (0)

#endif // ZHTTP_LOGGER_H_
