#ifndef ZHTTP_LOGGER_H_
#define ZHTTP_LOGGER_H_

#include "zlog/logger.h"

#include <string>
#include <utility>

namespace zhttp {

struct LoggerInitOptions {
    zlog::LogLevel::value level = zlog::LogLevel::value::DEBUG;
    bool async = true;
    std::string formatter = "[%d{%H:%M:%S}][%c][%p]%T%m%n";
    std::string sink = "stdout";
    std::string file_path;
};

/**
 * @brief 初始化 zhttp 专属日志器
 * @param options 初始化选项，支持日志级别、同步/异步、格式与落地方向
 */
void init_logger(const LoggerInitOptions &options);

/**
 * @brief 兼容旧接口：仅指定日志级别。
 */
void init_logger(zlog::LogLevel::value level = zlog::LogLevel::value::DEBUG);

/**
 * @brief 获取 zhttp 日志器
 * @return 全局日志器指针
 */
zlog::Logger *get_logger();
zlog::Logger::ptr get_logger_ptr();

template <typename... Args>
inline void log_debug(const char *file, int line, const char *fmt,
                      Args &&...args) {
    auto logger = get_logger_ptr();
    if (!logger) {
        return;
    }
    logger->debug(file, line, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void log_info(const char *file, int line, const char *fmt,
                     Args &&...args) {
    auto logger = get_logger_ptr();
    if (!logger) {
        return;
    }
    logger->info(file, line, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void log_warn(const char *file, int line, const char *fmt,
                     Args &&...args) {
    auto logger = get_logger_ptr();
    if (!logger) {
        return;
    }
    logger->warning(file, line, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void log_error(const char *file, int line, const char *fmt,
                      Args &&...args) {
    auto logger = get_logger_ptr();
    if (!logger) {
        return;
    }
    logger->error(file, line, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void log_fatal(const char *file, int line, const char *fmt,
                      Args &&...args) {
    auto logger = get_logger_ptr();
    if (!logger) {
        return;
    }
    logger->fatal(file, line, fmt, std::forward<Args>(args)...);
}

} // namespace zhttp

// 便捷日志宏，统一走 zhttp 自己的日志器实例。
#define ZHTTP_LOG_DEBUG(fmt, ...)                                              \
    ::zhttp::log_debug(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define ZHTTP_LOG_INFO(fmt, ...)                                               \
    ::zhttp::log_info(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define ZHTTP_LOG_WARN(fmt, ...)                                               \
    ::zhttp::log_warn(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define ZHTTP_LOG_ERROR(fmt, ...)                                              \
    ::zhttp::log_error(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define ZHTTP_LOG_FATAL(fmt, ...)                                              \
    ::zhttp::log_fatal(__FILE__, __LINE__, fmt, ##__VA_ARGS__)

#endif // ZHTTP_LOGGER_H_
