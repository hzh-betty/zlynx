#ifndef ZLOG_LOGGER_H_
#define ZLOG_LOGGER_H_
/**
 * @brief 日志器模块
 * 实现同步和异步日志器，以及日志器管理功能
 */

#include <mutex>
#include <unordered_map>
#include <utility>

#include <fmt/format.h>

#include "format.h"
#include "level.h"
#include "looper.h"
#include "sink.h"

namespace zlog {
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
   * @param loggerName 日志器名称
   * @param limitLevel 日志等级限制
   * @param formatter 日志格式化器
   * @param sinks 日志落地器列表
   */
  Logger(const char *loggerName, LogLevel::value limitLevel,
         Formatter::ptr formatter, std::vector<LogSink::ptr> &sinks);

  /**
   * @brief 获取日志器名称
   * @return 日志器名称字符串
   */
  std::string getName() const { return loggerName_; }

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
  void logImpl(Level level, const char *file, size_t line, const char *fmt,
               Args &&...args) {
    logImplHelper(level, file, line, fmt, std::forward<Args>(args)...);
  }

  /**
   * @brief 记录 DEBUG 级别日志
   */
  template <typename... Args>
  void debug(const char *file, size_t line, const char *fmt, Args &&...args) {
    logImpl(LogLevel::value::DEBUG, file, line, fmt,
            std::forward<Args>(args)...);
  }

  /**
   * @brief 记录 INFO 级别日志
   */
  template <typename... Args>
  void info(const char *file, size_t line, const char *fmt, Args &&...args) {
    logImpl(LogLevel::value::INFO, file, line, fmt,
            std::forward<Args>(args)...);
  }

  /**
   * @brief 记录 WARNING 级别日志
   */
  template <typename... Args>
  void warning(const char *file, size_t line, const char *fmt, Args &&...args) {
    logImpl(LogLevel::value::WARNING, file, line, fmt,
            std::forward<Args>(args)...);
  }

  /**
   * @brief 记录 ERROR 级别日志
   */
  template <typename... Args>
  void error(const char *file, size_t line, const char *fmt, Args &&...args) {
    logImpl(LogLevel::value::ERROR, file, line, fmt,
            std::forward<Args>(args)...);
  }

  /**
   * @brief 记录 FATAL 级别日志
   */
  template <typename... Args>
  void fatal(const char *file, size_t line, const char *fmt, Args &&...args) {
    logImpl(LogLevel::value::FATAL, file, line, fmt,
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
  void logImplHelper(const LogLevel::value level, const char *file,
                     const size_t line, const char *fmt, Args &&...args) {
    if (level < limitLevel_)
      return;

    // 线程局部缓冲区，预分配内存并复用
    thread_local fmt::memory_buffer fmtBuffer;
    fmtBuffer.clear(); // 清空旧数据

    // 格式化到缓冲区
    fmt::vformat_to(std::back_inserter(fmtBuffer), fmt,
                    fmt::make_format_args((args)...));

    // 添加终止符（如需要C风格字符串）
    fmtBuffer.push_back('\0');

    // 使用缓冲区内容（例如输出或转换为字符串）
    serialize(level, file, line, fmtBuffer.data());
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
  const char *loggerName_;          // 日志器名称
  LogLevel::value limitLevel_;      // 日志等级限制
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
   * @param loggerName 日志器名称
   * @param limitLevel 日志等级限制
   * @param formatter 日志格式化器
   * @param sinks 日志落地器列表
   */
  SyncLogger(const char *loggerName, const LogLevel::value limitLevel,
             const Formatter::ptr &formatter, std::vector<LogSink::ptr> &sinks);

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
   * @param loggerName 日志器名称
   * @param limitLevel 日志等级限制
   * @param formatter 日志格式化器
   * @param sinks 日志落地器列表
   * @param looperType 异步类型
   * @param milliseco 最大等待时间
   */
  AsyncLogger(const char *loggerName, const LogLevel::value limitLevel,
              const Formatter::ptr &formatter, std::vector<LogSink::ptr> &sinks,
              AsyncType looperType, std::chrono::milliseconds milliseco);

protected:
  /**
   * @brief 异步日志输出实现
   * 将数据写入到缓冲区
   * @param data 日志数据
   * @param len 数据长度
   */
  void log(const char *data, const size_t len) override;

  /**
   * @brief 实际日志落地函数
   * 将数据从缓冲区中落地到各个sink
   * @param buffer 缓冲区
   */
  void reLog(const Buffer &buffer) const;

protected:
  AsyncLooper::ptr looper_; // 异步循环器
};

/**
 * @brief 日志器类型枚举
 */
enum class LoggerType {
  LOGGER_SYNC, // 同步日志器
  LOGGER_ASYNC // 异步日志器
};

/**
 * @brief 日志器建造者基类
 * 使用建造者模式降低用户使用成本
 */
class LoggerBuilder : public NonCopyable {
public:
  virtual ~LoggerBuilder() = default;

  /**
   * @brief 构造函数
   * 初始化默认配置
   */
  LoggerBuilder();

  /**
   * @brief 设置日志器类型
   * @param loggerType 日志器类型（同步/异步）
   */
  void buildLoggerType(const LoggerType loggerType);

  /**
   * @brief 启用非安全异步模式
   */
  void buildEnalleUnSafe();

  /**
   * @brief 设置日志器名称
   * @param loggerName 日志器名称
   */
  void buildLoggerName(const char *loggerName);

  /**
   * @brief 设置日志等级限制
   * @param limitLevel 日志等级
   */
  void buildLoggerLevel(LogLevel::value limitLevel);

  /**
   * @brief 设置异步等待时间
   * @param milliseco 等待时间（毫秒）
   */
  void buildWaitTime(const std::chrono::milliseconds milliseco);

  /**
   * @brief 设置日志格式化器
   * @param pattern 格式化字符串
   */
  void buildLoggerFormatter(const std::string &pattern);

  /**
   * @brief 添加日志落地器
   * @tparam SinkType 落地器类型
   * @tparam Args 构造参数类型
   * @param args 构造参数
   */
  template <typename SinkType, typename... Args>
  void buildLoggerSink(Args &&...args) {
    const LogSink::ptr psink =
        SinkFactory::create<SinkType>(std::forward<Args>(args)...);
    sinks_.push_back(psink);
  }

  /**
   * @brief 纯虚函数，由子类实现具体地构建逻辑
   * @return 构建的日志器智能指针
   */
  virtual Logger::ptr build() = 0;

protected:
  LoggerType loggerType_;               // 日志器类型
  const char *loggerName_ = nullptr;    // 日志器名称
  LogLevel::value limitLevel_;          // 日志等级限制
  Formatter::ptr formatter_;            // 日志格式化器
  std::vector<LogSink::ptr> sinks_;     // 日志落地器列表
  AsyncType looperType_;                // 异步类型
  std::chrono::milliseconds milliseco_; // 最大等待时间
};

/**
 * @brief 局部日志器建造者
 * 创建局部作用域的日志器
 */
class LocalLoggerBuilder final : public LoggerBuilder {
public:
  /**
   * @brief 构建局部日志器
   * @return 日志器智能指针
   */
  LocalLoggerBuilder() = default;
  Logger::ptr build() override;
};

/**
 * @brief 全局日志器管理器
 * 负责管理所有日志器并提供全局访问接口
 */
class LoggerManager {
public:
  /**
   * @brief 获取单例实例
   * @return 日志器管理器实例引用
   */
  static LoggerManager &getInstance() {
    static LoggerManager eton;
    return eton;
  }

  /**
   * @brief 添加日志器
   * @param logger 日志器智能指针
   */
  void addLogger(Logger::ptr &logger);

  /**
   * @brief 检查日志器是否存在
   * @param name 日志器名称
   * @return 存在返回true，否则返回false
   */
  bool hasLogger(const std::string &name);

  /**
   * @brief 获取指定名称的日志器
   * @param name 日志器名称
   * @return 日志器智能指针，不存在则返回空指针
   */
  Logger::ptr getLogger(const std::string &name);

  /**
   * @brief 获取根日志器
   * @return 根日志器智能指针
   */
  Logger::ptr rootLogger();

private:
  /**
   * @brief 私有构造函数
   * 初始化根日志器
   */
  LoggerManager();

private:
  std::mutex mutex_;                                     // 互斥锁
  Logger::ptr rootLogger_;                               // 默认根日志器
  std::unordered_map<std::string, Logger::ptr> loggers_; // 日志器映射表
};

/**
 * @brief 全局日志器建造者
 * 创建全局作用域的日志器并自动注册到管理器
 */
class GlobalLoggerBuilder final : public LoggerBuilder {
public:
  /**
   * @brief 构建全局日志器
   * @return 日志器智能指针
   */
  GlobalLoggerBuilder() = default;
  Logger::ptr build() override;
};

} // namespace zlog

#endif // ZLOG_LOGGER_H_
