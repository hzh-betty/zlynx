#ifndef ZLOG_FORMAT_H_
#define ZLOG_FORMAT_H_
#include "message.h"
#include <memory>
#include <vector>
#include <fmt/core.h>
#include <fmt/color.h>

namespace zlog
{
    /**
     * @brief 格式化项抽象基类
     * 定义了格式化项的通用接口
     */
    class FormatItem
    {
    public:
        virtual ~FormatItem() = default;

        using prt = std::shared_ptr<FormatItem>;

        /**
         * @brief 格式化日志消息
         * @param buffer 输出缓冲区
         * @param msg 日志消息
         */
        virtual void format(fmt::memory_buffer &buffer, const LogMessage &msg) = 0;
    };

    /**
     * @brief 消息格式化项
     * 格式化日志的主体消息内容
     */
    class MessageFormatItem final : public FormatItem
    {
    public:
        void format(fmt::memory_buffer &buffer, const LogMessage &msg) override;
    };

    /**
     * @brief 等级格式化项
     * 格式化日志等级信息
     */
    class LevelFormatItem : public FormatItem
    {
    public:
        void format(fmt::memory_buffer &buffer, const LogMessage &msg) override;
    };

    static const std::string timeFormatDefault = "%H:%M:%S"; // 默认时间输出格式

    /**
     * @brief 时间格式化项
     * 格式化日志时间信息
     */
    class TimeFormatItem final : public FormatItem
    {
    public:
        /**
         * @brief 构造函数
         * @param timeFormat 时间格式字符串，默认为 "%H:%M:%S"
         */
        explicit TimeFormatItem(std::string timeFormat = timeFormatDefault);

        void format(fmt::memory_buffer &buffer, const LogMessage &msg) override;

    protected:
        std::string timeFormat_;    ///< 时间格式字符串
    };

    /**
     * @brief 文件名格式化项
     * 格式化源码文件名
     */
    class FileFormatItem final : public FormatItem
    {
    public:
        void format(fmt::memory_buffer &buffer, const LogMessage &msg) override;
    };

    /**
     * @brief 行号格式化项
     * 格式化源码行号
     */
    class LineFormatItem final : public FormatItem
    {
    public:
        void format(fmt::memory_buffer &buffer, const LogMessage &msg) override;
    };

    /**
     * @brief 线程ID格式化项
     * 格式化线程ID信息
     */
    class ThreadIdFormatItem final : public FormatItem
    {
    public:
        void format(fmt::memory_buffer &buffer, const LogMessage &msg) override;
    };

    /**
     * @brief 日志器名称格式化项
     * 格式化日志器名称
     */
    class LoggerFormatItem final : public FormatItem
    {
    public:
        void format(fmt::memory_buffer &buffer, const LogMessage &msg) override;
    };

    /**
     * @brief 制表符格式化项
     * 在日志中添加制表符
     */
    class TabFormatItem final : public FormatItem
    {
    public:
        void format(fmt::memory_buffer &buffer, const LogMessage &msg) override;
    };

    /**
     * @brief 换行符格式化项
     * 在日志中添加换行符
     */
    class NLineFormatItem final : public FormatItem
    {
    public:
        void format(fmt::memory_buffer &buffer, const LogMessage &msg) override;
    };

    /**
     * @brief 其他字符格式化项
     * 处理格式化字符串中的普通字符
     */
    class OtherFormatItem final : public FormatItem
    {
    public:
        /**
         * @brief 构造函数
         * @param str 要输出的字符串
         */
        explicit OtherFormatItem(std::string str);

        void format(fmt::memory_buffer &buffer, const LogMessage &msg) override;

    protected:
        std::string str_;   ///< 要输出的字符串
    };

    /**
     * @brief 日志格式化器
     * 解析格式化字符串并生成相应的格式化项
     *
     * 格式化字符串说明：
     * %d 表示日期，可包含子格式{%H:%M:%S}
     * %t 线程ID
     * %c 日志器名称
     * %f 源码文件名
     * %l 行号
     * %p 日志级别
     * %T 制表符缩进
     * %m 主体消息
     * %n 换行符
     */
    class Formatter
    {
    public:
        using ptr = std::shared_ptr<Formatter>;

        /**
         * @brief 构造函数
         * @param pattern 格式化字符串
         */
        explicit Formatter(std::string pattern = "[%d{%H:%M:%S}][%t][%c][%f:%l][%p]%T%m%n");

        /**
         * @brief 格式化日志消息
         * @param buffer 输出缓冲区
         * @param msg 日志消息
         */
        void format(fmt::memory_buffer &buffer, const LogMessage &msg) const;

    protected:
        /**
         * @brief 解析格式化字符串
         * @return 解析成功返回true，否则返回false
         */
        bool parsePattern();

        /**
         * @brief 根据格式化字符创建对应的格式化项
         * @param key 格式化字符
         * @param val 格式化参数
         * @return 格式化项智能指针
         */
        static FormatItem::prt createItem(const std::string &key, const std::string &val);

    protected:
        std::string pattern_;                       ///< 格式化字符串
        std::vector<FormatItem::prt> items_;       ///< 格式化项列表
    };
} // namespace zlog

#endif // ZLOG_FORMAT_H_
