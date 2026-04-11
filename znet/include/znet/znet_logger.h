#ifndef ZNET_LOGGER_H_
#define ZNET_LOGGER_H_

#include "zlog/logger.h"

#include <string>
#include <utility>

namespace znet {

struct LoggerInitOptions {
    zlog::LogLevel::value level = zlog::LogLevel::value::DEBUG;
    bool async = true;
    std::string formatter = "[%d{%H:%M:%S}][%c][%p]%T%m%n";
    std::string sink = "stdout";
    std::string file_path;
};

/**
 * @brief 初始化 znet 专属日志器。
 * @param options 初始化选项，支持日志级别、同步/异步、格式与落地方向。
 */
void init_logger(const LoggerInitOptions &options);

/**
 * @brief 兼容旧接口：仅指定日志级别。
 */
void init_logger(zlog::LogLevel::value level = zlog::LogLevel::value::DEBUG);

/**
 * @brief 获取 znet 默认日志器。
 * @return 日志器裸指针；若初始化失败可能返回 nullptr。
 */
zlog::Logger *get_logger();

template <typename... Args>
inline void log_debug(const char *file, int line, const char *fmt,
                      Args &&...args) {
    auto *logger = get_logger();
    if (!logger) {
        return;
    }
    logger->debug(file, line, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void log_info(const char *file, int line, const char *fmt,
                     Args &&...args) {
    auto *logger = get_logger();
    if (!logger) {
        return;
    }
    logger->info(file, line, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void log_warn(const char *file, int line, const char *fmt,
                     Args &&...args) {
    auto *logger = get_logger();
    if (!logger) {
        return;
    }
    logger->warning(file, line, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void log_error(const char *file, int line, const char *fmt,
                      Args &&...args) {
    auto *logger = get_logger();
    if (!logger) {
        return;
    }
    logger->error(file, line, fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void log_fatal(const char *file, int line, const char *fmt,
                      Args &&...args) {
    auto *logger = get_logger();
    if (!logger) {
        return;
    }
    logger->fatal(file, line, fmt, std::forward<Args>(args)...);
}
} // namespace znet

// 便捷日志宏：自动补充文件和行号。
#define ZNET_LOG_DEBUG(...) ::znet::log_debug(__FILE__, __LINE__, __VA_ARGS__)
#define ZNET_LOG_INFO(...) ::znet::log_info(__FILE__, __LINE__, __VA_ARGS__)
#define ZNET_LOG_WARN(...) ::znet::log_warn(__FILE__, __LINE__, __VA_ARGS__)
#define ZNET_LOG_ERROR(...) ::znet::log_error(__FILE__, __LINE__, __VA_ARGS__)
#define ZNET_LOG_FATAL(...) ::znet::log_fatal(__FILE__, __LINE__, __VA_ARGS__)

#endif // ZNET_LOGGER_H_
