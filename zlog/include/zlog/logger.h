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

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <stdexcept>
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

    /** @brief 等待此前已接收的日志完成输出并刷新所有 sink；失败时抛出异常。 */
    virtual void flush() = 0;

    /** @brief 停止接收、排空并刷新；清理后报告错误，重复关闭无操作。 */
    virtual void close() = 0;

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
    void log_impl(Level level, const char *file, size_t line, fmt::string_view fmt,
                  Args &&...args) {
        log_impl_helper(level, file, line, fmt, std::forward<Args>(args)...);
    }

    /**
     * @brief 记录 DEBUG 级别日志
     */
    template <typename... Args>
    void debug(const char *file, size_t line, fmt::string_view fmt, Args &&...args) {
        log_impl(LogLevel::value::DEBUG, file, line, fmt,
                 std::forward<Args>(args)...);
    }

    /**
     * @brief 记录 INFO 级别日志
     */
    template <typename... Args>
    void info(const char *file, size_t line, fmt::string_view fmt, Args &&...args) {
        log_impl(LogLevel::value::INFO, file, line, fmt,
                 std::forward<Args>(args)...);
    }

    /**
     * @brief 记录 WARNING 级别日志
     */
    template <typename... Args>
    void warning(const char *file, size_t line, fmt::string_view fmt,
                 Args &&...args) {
        log_impl(LogLevel::value::WARNING, file, line, fmt,
                 std::forward<Args>(args)...);
    }

    /**
     * @brief 记录 ERROR 级别日志
     */
    template <typename... Args>
    void error(const char *file, size_t line, fmt::string_view fmt, Args &&...args) {
        log_impl(LogLevel::value::ERROR, file, line, fmt,
                 std::forward<Args>(args)...);
    }

    /**
     * @brief 记录 FATAL 级别日志
     */
    template <typename... Args>
    void fatal(const char *file, size_t line, fmt::string_view fmt, Args &&...args) {
        log_impl(LogLevel::value::FATAL, file, line, fmt,
                 std::forward<Args>(args)...);
    }

  private:
    // 缓存只借给当前最外层调用；异常退出也必须归还占用标记。
    class BufferUse {
      public:
        explicit BufferUse(bool &in_use) noexcept : in_use_(in_use) { in_use_ = true; }
        ~BufferUse() { in_use_ = false; }
        BufferUse(const BufferUse &) = delete;
        BufferUse &operator=(const BufferUse &) = delete;
      private:
        bool &in_use_;
    };

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
                         const size_t line, fmt::string_view fmt, Args &&...args) {
        if (closed_.load(std::memory_order_acquire)) {
            throw std::runtime_error("logger is closed");
        }
        if (level < limit_level_)
            return;

        // 线程局部缓冲区复用容量，普通调用不重复构造或释放大消息的内存。
        thread_local fmt::memory_buffer fmt_buffer;
        thread_local bool buffer_in_use = false;
        const auto write = [&](fmt::memory_buffer &buffer) {
            buffer.clear(); // 清空旧数据，保留已经扩容的容量。
            // 格式化到缓冲区。
            fmt::vformat_to(std::back_inserter(buffer), fmt,
                            fmt::make_format_args((args)...));
            // 视图只在本次序列化期间借用缓冲区，保留长度和内嵌零字节。
            serialize(level, file, line,
                      fmt::string_view(buffer.data(), buffer.size()));
        };
        if (buffer_in_use) {
            // 自定义参数格式化或 sink 触发嵌套日志时，不覆盖外层还在使用的数据。
            fmt::memory_buffer nested_buffer;
            write(nested_buffer);
        } else {
            BufferUse use(buffer_in_use);
            write(fmt_buffer);
        }
    }

    /**
     * @brief 序列化日志消息
     * @param level 日志等级
     * @param file 源文件名
     * @param line 源文件行号
     * @param data 日志数据
     */
    void serialize(LogLevel::value level, const char *file, size_t line,
                   fmt::string_view data);

    /**
     * @brief 纯虚函数，由子类实现具体的日志输出逻辑
     * @param data 日志数据
     * @param len 数据长度
     */
    virtual void log(const char *data, size_t len) = 0;

    void write_sinks(const char *data, size_t len);
    void flush_sinks();
    bool is_active_on_current_thread() const;
    void close_noexcept() noexcept;

  protected:
    std::mutex mutex_;                // 互斥锁
    std::atomic<bool> closed_{false};  // 关闭后拒绝新日志
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

    ~SyncLogger() override;
    void flush() override;
    void close() override;

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
    void flush() override;
    void close() override;

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
