#ifndef ZNET_LOGGER_H_
#define ZNET_LOGGER_H_

#include "zlog/logger.h"

namespace znet {

/**
 * @brief 初始化 znet 专属日志器。
 * @param level 日志等级；初始化 znet 时会同步初始化 zco。
 */
void init_logger(zlog::LogLevel::value level = zlog::LogLevel::value::INFO);

/**
 * @brief 获取 znet 默认日志器。
 * @return 日志器智能指针，若初始化失败可能返回空。
 */
zlog::Logger::ptr get_logger_ptr();

bool should_log(zlog::LogLevel::value level);
} // namespace znet

// 便捷日志宏：自动补充文件和行号。
#define ZNET_LOG_DEBUG(...)                                                    \
    do {                                                                       \
        if (::znet::should_log(::zlog::LogLevel::value::DEBUG)) {              \
            auto znet_logger__ = ::znet::get_logger_ptr();                     \
            if (znet_logger__) {                                               \
                znet_logger__->debug(__FILE__, __LINE__, __VA_ARGS__);         \
            }                                                                  \
        }                                                                      \
    } while (0)
#define ZNET_LOG_INFO(...)                                                     \
    do {                                                                       \
        if (::znet::should_log(::zlog::LogLevel::value::INFO)) {               \
            auto znet_logger__ = ::znet::get_logger_ptr();                     \
            if (znet_logger__) {                                               \
                znet_logger__->info(__FILE__, __LINE__, __VA_ARGS__);          \
            }                                                                  \
        }                                                                      \
    } while (0)
#define ZNET_LOG_WARN(...)                                                     \
    do {                                                                       \
        if (::znet::should_log(::zlog::LogLevel::value::WARNING)) {            \
            auto znet_logger__ = ::znet::get_logger_ptr();                     \
            if (znet_logger__) {                                               \
                znet_logger__->warning(__FILE__, __LINE__, __VA_ARGS__);       \
            }                                                                  \
        }                                                                      \
    } while (0)
#define ZNET_LOG_ERROR(...)                                                    \
    do {                                                                       \
        if (::znet::should_log(::zlog::LogLevel::value::ERROR)) {              \
            auto znet_logger__ = ::znet::get_logger_ptr();                     \
            if (znet_logger__) {                                               \
                znet_logger__->error(__FILE__, __LINE__, __VA_ARGS__);         \
            }                                                                  \
        }                                                                      \
    } while (0)
#define ZNET_LOG_FATAL(...)                                                    \
    do {                                                                       \
        if (::znet::should_log(::zlog::LogLevel::value::FATAL)) {              \
            auto znet_logger__ = ::znet::get_logger_ptr();                     \
            if (znet_logger__) {                                               \
                znet_logger__->fatal(__FILE__, __LINE__, __VA_ARGS__);         \
            }                                                                  \
        }                                                                      \
    } while (0)

#endif // ZNET_LOGGER_H_
