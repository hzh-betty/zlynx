#ifndef ZLOG_SINK_H_
#define ZLOG_SINK_H_

#include <fstream>
#include <string>
#include <utility>

#include <fmt/core.h>
#include <fmt/os.h>
#include <fmt/ostream.h>

/**
 * @brief 日志落地模块
 * 实现日志输出到不同目标（控制台、文件、滚动文件）
 * 使用工厂模式进行创建与表示的分离
 */
namespace zlog {
/**
 * @brief 日志落地抽象基类
 * 定义了日志输出的通用接口
 */
class LogSink {
  public:
    using ptr = std::shared_ptr<LogSink>;
    LogSink() = default;
    virtual ~LogSink() = default;

    /**
     * @brief 输出日志数据
     * @param data 日志数据指针
     * @param len 数据长度
     */
    virtual void log(const char *data, size_t len) = 0;
};

/**
 * @brief 标准输出日志落地器
 * 将日志输出到控制台
 */
class StdOutSink final : public LogSink {
  public:
    void log(const char *data, size_t len) override;
};

/**
 * @brief 文件日志落地器
 * 将日志输出到指定文件
 */
class FileSink final : public LogSink {
  public:
    /**
     * @brief 构造函数
     * @param pathname 文件路径
     * @param auto_flush 是否每次写入后自动flush，默认false以提高性能
     */
    explicit FileSink(std::string pathname, bool auto_flush = false);

    void log(const char *data, size_t len) override;

  protected:
    std::string pathname_; // 文件路径
    std::ofstream ofs_;    // 输出文件流
    bool auto_flush_;      // 是否自动flush
};

/**
 * @brief 按大小滚动的文件日志落地器
 * 当文件大小超过限制时自动创建新文件
 */
class RollBySizeSink final : public LogSink {
  public:
    /**
     * @brief 构造函数
     * @param basename 文件基础名称
     * @param max_size 最大文件大小（字节）
     * @param auto_flush 是否每次写入后自动flush，默认false
     */
    RollBySizeSink(std::string basename, size_t max_size,
                   bool auto_flush = false);

    void log(const char *data, size_t len) override;

  protected:
    /**
     * @brief 创建新文件
     * @return 新文件的完整路径
     */
    std::string create_new_file();

    /**
     * @brief 滚动到新文件
     * 关闭当前文件，创建新文件
     */
    void roll_over();

    std::string basename_; // 文件基础名称
    std::ofstream ofs_;    // 输出文件流
    size_t max_size_;      // 最大文件大小
    size_t cur_size_;      // 当前文件大小
    size_t name_count_;    // 文件名计数器
    bool auto_flush_;      // 是否自动flush
};

/**
 * @brief 日志落地器工厂类
 * 使用工厂模式创建不同类型的日志落地器
 */
class SinkFactory {
  public:
    /**
     * @brief 创建日志落地器
     * @tparam SinkType 落地器类型
     * @tparam Args 构造参数类型
     * @param args 构造参数
     * @return 日志落地器智能指针
     */
    template <typename SinkType, typename... Args>
    static LogSink::ptr create(Args &&...args) {
        return std::make_shared<SinkType>(std::forward<Args>(args)...);
    }
};
} // namespace zlog

#endif // ZLOG_SINK_H_
