/**
 * @file logger_builder.h
 * @brief 日志器构建接口。
 */

#ifndef ZLOG_LOGGER_BUILDER_H_
#define ZLOG_LOGGER_BUILDER_H_

#include "zlog/logger.h"

namespace zlog {
/**
 * @brief 日志器类型枚举
 */
enum class LoggerType {
    LOGGER_SYNC, // 同步日志器
    LOGGER_ASYNC // 异步日志器
};

/**
 * @brief 日志器建造者
 * 使用建造者模式降低用户使用成本
 */
class LoggerBuilder {
  public:
    ~LoggerBuilder() = default;

    /**
     * @brief 构造函数
     * 初始化默认配置
     */
    LoggerBuilder();
    LoggerBuilder(const LoggerBuilder &) = delete;
    LoggerBuilder &operator=(const LoggerBuilder &) = delete;

    /**
     * @brief 设置日志器类型
     * @param logger_type 日志器类型（同步/异步）
     */
    void build_logger_type(const LoggerType logger_type);

    /**
     * @brief 启用非安全异步模式
     */
    void build_enable_unsafe();

    /**
     * @brief 设置日志器名称
     * @param logger_name 日志器名称
     */
    void build_logger_name(const char *logger_name);

    /** @brief 按长度复制名称；视图只需在调用期间有效。 */
    void build_logger_name(fmt::string_view logger_name);

    /**
     * @brief 设置日志等级限制
     * @param limit_level 日志等级
     */
    void build_logger_level(LogLevel::value limit_level);

    /**
     * @brief 设置异步等待时间
     * @param milliseco 等待时间（毫秒）
     */
    void build_wait_time(const std::chrono::milliseconds milliseco);

    /**
     * @brief 设置日志格式化器
     * @param pattern 格式化字符串
     */
    void build_logger_formatter(const std::string &pattern);

    /**
     * @brief 添加日志落地器
     * @tparam SinkType 落地器类型
     * @tparam Args 构造参数类型
     * @param args 构造参数
     */
    template <typename SinkType, typename... Args>
    void build_logger_sink(Args &&...args) {
        const LogSink::ptr psink =
            std::make_shared<SinkType>(std::forward<Args>(args)...);
        sinks_.push_back(psink);
    }

    /** @brief 构建日志器，不注册到全局管理器。 */
    Logger::ptr build();

  private:
    LoggerType logger_type_;              // 日志器类型
    std::string logger_name_;             // 自有名称
    bool has_logger_name_ = false;        // 区分未设置名称与空名称
    LogLevel::value limit_level_;         // 日志等级限制
    Formatter::ptr formatter_;            // 日志格式化器
    std::vector<LogSink::ptr> sinks_;     // 日志落地器列表
    AsyncType looper_type_;               // 异步类型
    std::chrono::milliseconds milliseco_; // 最大等待时间
};

} // namespace zlog

#endif // ZLOG_LOGGER_BUILDER_H_
