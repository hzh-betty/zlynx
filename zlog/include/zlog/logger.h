/**
 * @file logger.h
 * @brief logger 定义。
 * @author hzh-betty
 */

#ifndef ZLOG_LOGGER_H_
#define ZLOG_LOGGER_H_
/**
 * @brief 日志器模块
 * 实现同步和异步日志器的记录接口
 */

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "zlog/format.h"
#include "zlog/level.h"
#include "zlog/sink.h"

namespace zlog {
namespace detail {
class AsyncLooper;
}

/** @brief 异步缓冲策略；两种策略达到容量上限时都会阻塞等待。 */
enum class AsyncType {
    ASYNC_SAFE,  // 固定容量，等待可用空间
    ASYNC_UNSAFE // 允许扩容，达到上限后等待可用空间
};

/**
 * @brief 日志器抽象基类
 * 提供日志记录的核心功能，支持模板化的日志接口
 */
class Logger {
  public:
    virtual ~Logger() = default;

    using ptr = std::shared_ptr<Logger>;

    /**
     * @brief 构造函数
     * @param logger_name 日志器名称
     * @param limit_level 日志等级限制
     * @param formatter 日志格式化器
     * @param sinks 日志落地器列表
     */
    Logger(std::string logger_name, LogLevel::value limit_level,
           Formatter::ptr formatter, const std::vector<LogSink::ptr> &sinks);

    /**
     * @brief 获取日志器名称
     * @return 日志器名称字符串
     */
    std::string get_name() const { return logger_name_; }

    /**
     * @brief 日志记录模板接口
     * @tparam Level 日志等级类型
     * @tparam Args 参数类型包
     * @param level 日志等级
     * @param file 源文件名
     * @param line 源文件行号
     * @param fmt 格式化字符串
     * @param args 格式化参数
     */
    template <typename Level, typename... Args>
    void log_impl(Level level, const char *file, size_t line, const char *fmt,
                  Args &&...args) {
        log_impl_helper(level, file, line, fmt, std::forward<Args>(args)...);
    }

    /**
     * @brief 记录 DEBUG 级别日志
     */
    template <typename... Args>
    void debug(const char *file, size_t line, const char *fmt, Args &&...args) {
        log_impl(LogLevel::value::DEBUG, file, line, fmt,
                 std::forward<Args>(args)...);
    }

    /**
     * @brief 记录 INFO 级别日志
     */
    template <typename... Args>
    void info(const char *file, size_t line, const char *fmt, Args &&...args) {
        log_impl(LogLevel::value::INFO, file, line, fmt,
                 std::forward<Args>(args)...);
    }

    /**
     * @brief 记录 WARNING 级别日志
     */
    template <typename... Args>
    void warning(const char *file, size_t line, const char *fmt,
                 Args &&...args) {
        log_impl(LogLevel::value::WARNING, file, line, fmt,
                 std::forward<Args>(args)...);
    }

    /**
     * @brief 记录 ERROR 级别日志
     */
    template <typename... Args>
    void error(const char *file, size_t line, const char *fmt, Args &&...args) {
        log_impl(LogLevel::value::ERROR, file, line, fmt,
                 std::forward<Args>(args)...);
    }

    /**
     * @brief 记录 FATAL 级别日志
     */
    template <typename... Args>
    void fatal(const char *file, size_t line, const char *fmt, Args &&...args) {
        log_impl(LogLevel::value::FATAL, file, line, fmt,
                 std::forward<Args>(args)...);
    }

  protected:
    /**
     * @brief 日志记录辅助函数
     * @tparam Args 参数类型包
     * @param level 日志等级
     * @param file 源文件名
     * @param line 源文件行号
     * @param fmt 格式化字符串
     * @param args 格式化参数
     */
    template <typename... Args>
    void log_impl_helper(const LogLevel::value level, const char *file,
                         const size_t line, const char *fmt, Args &&...args) {
        if (level < limit_level_)
            return;

        // 线程局部缓冲区，预分配内存并复用
        thread_local fmt::memory_buffer fmt_buffer;
        fmt_buffer.clear(); // 清空旧数据

        // 格式化到缓冲区
        fmt::vformat_to(std::back_inserter(fmt_buffer), fmt,
                        fmt::make_format_args((args)...));

        // 添加终止符（如需要C风格字符串）
        fmt_buffer.push_back('\0');

        // 使用缓冲区内容（例如输出或转换为字符串）
        serialize(level, file, line, fmt_buffer.data());
    }

    /**
     * @brief 序列化日志消息
     * @param level 日志等级
     * @param file 源文件名
     * @param line 源文件行号
     * @param data 日志数据
     */
    void serialize(LogLevel::value level, const char *file, size_t line,
                   const char *data);

    /**
     * @brief 纯虚函数，由子类实现具体的日志输出逻辑
     * @param data 日志数据
     * @param len 数据长度
     */
    virtual void log(const char *data, size_t len) = 0;

  protected:
    std::mutex mutex_;                // 互斥锁
    const std::string logger_name_;   // 自有名称，生命周期与日志器一致
    LogLevel::value limit_level_;     // 日志等级限制
    Formatter::ptr formatter_;        // 日志格式化器
    std::vector<LogSink::ptr> sinks_; // 日志落地器列表
};

/**
 * @brief 同步日志器
 * 直接通过日志落地模块进行同步日志输出
 */
class SyncLogger final : public Logger {
  public:
    /**
     * @brief 构造函数
     * @param logger_name 日志器名称
     * @param limit_level 日志等级限制
     * @param formatter 日志格式化器
     * @param sinks 日志落地器列表
     */
    SyncLogger(std::string logger_name, const LogLevel::value limit_level,
               const Formatter::ptr &formatter,
               const std::vector<LogSink::ptr> &sinks);

  protected:
    /**
     * @brief 同步日志输出实现
     * @param data 日志数据
     * @param len 数据长度
     */
    void log(const char *data, const size_t len) override;
};

/**
 * @brief 异步日志器
 * 通过异步循环器实现异步日志输出
 */
class AsyncLogger final : public Logger {
  public:
    /**
     * @brief 构造函数
     * @param logger_name 日志器名称
     * @param limit_level 日志等级限制
     * @param formatter 日志格式化器
     * @param sinks 日志落地器列表
     * @param looper_type 异步类型
     * @param milliseco 最大等待时间
     */
    AsyncLogger(std::string logger_name, const LogLevel::value limit_level,
                const Formatter::ptr &formatter,
                const std::vector<LogSink::ptr> &sinks, AsyncType looper_type,
                std::chrono::milliseconds milliseco);

    // 在实现文件中销毁工作线程，排空后才释放基类持有的 sink。
    ~AsyncLogger() override;

  protected:
    /**
     * @brief 异步日志输出实现
     * 将数据写入到缓冲区
     * @param data 日志数据
     * @param len 数据长度
     */
    void log(const char *data, const size_t len) override;

  private:
    std::unique_ptr<detail::AsyncLooper> looper_; // 独占异步循环器
};

} // namespace zlog

#endif // ZLOG_LOGGER_H_
