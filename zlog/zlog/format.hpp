#pragma once
#include "message.hpp"
#include <ctime>
#include <memory>
#include <utility>
#include <vector>
#include <sstream>
#include <cassert>
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
        void format(fmt::memory_buffer &buffer, const LogMessage &msg) override
        {
            fmt::format_to(std::back_inserter(buffer), "{}", msg.payload_);
        }
    };

    /**
     * @brief 等级格式化项
     * 格式化日志等级信息
     */
    class LevelFormatItem : public FormatItem
    {
    public:
        void format(fmt::memory_buffer &buffer, const LogMessage &msg) override
        {
            std::string levelstr = LogLevel::toString(msg.level_);
            fmt::format_to(std::back_inserter(buffer), "{}", levelstr);
        }
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
        explicit TimeFormatItem(std::string timeFormat = timeFormatDefault)
            : timeFormat_(std::move(timeFormat))
        {
        }

        void format(fmt::memory_buffer &buffer, const LogMessage &msg) override
        {
            thread_local std::string cached_time;
            thread_local time_t last_cached = 0;

            if (last_cached != msg.curtime_)
            {
                struct tm lt{};
#ifdef _WIN32
                localtime_s(&lt, &msg.curtime_);
#else
                localtime_r(&msg.curtime_, &lt);
#endif

                // 使用临时缓冲区
                char buf[64];
                const size_t len = strftime(buf, sizeof(buf), timeFormat_.c_str(), &lt);
                if (len > 0)
                {
                    cached_time.assign(buf, len);
                    last_cached = msg.curtime_;
                }
                else
                {
                    cached_time = "InvalidTime";
                }
            }
            fmt::format_to(std::back_inserter(buffer), "{}", cached_time);
        }

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
        void format(fmt::memory_buffer &buffer, const LogMessage &msg) override
        {
            fmt::format_to(std::back_inserter(buffer), "{}", msg.file_);
        }
    };

    /**
     * @brief 行号格式化项
     * 格式化源码行号
     */
    class LineFormatItem final : public FormatItem
    {
    public:
        void format(fmt::memory_buffer &buffer, const LogMessage &msg) override
        {
            fmt::format_to(std::back_inserter(buffer), "{}", msg.line_);
        }
    };

    thread_local threadId id_cached{};
    thread_local std::string tidStr;

    /**
     * @brief 线程ID格式化项
     * 格式化线程ID信息
     */
    class ThreadIdFormatItem final : public FormatItem
    {
    public:
        void format(fmt::memory_buffer &buffer, const LogMessage &msg) override
        {
            // 当缓存的ID与当前消息ID不同时才更新
            if(id_cached != msg.tid_)
            {
                id_cached = msg.tid_;
                std::stringstream ss;
                ss << id_cached;
                tidStr = ss.str();
            }

            fmt::format_to(std::back_inserter(buffer), "{}", tidStr);
        }
    };

    /**
     * @brief 日志器名称格式化项
     * 格式化日志器名称
     */
    class LoggerFormatItem final : public FormatItem
    {
    public:
        void format(fmt::memory_buffer &buffer, const LogMessage &msg) override
        {
            fmt::format_to(std::back_inserter(buffer), "{}", msg.loggerName_);
        }
    };

    /**
     * @brief 制表符格式化项
     * 在日志中添加制表符
     */
    class TabFormatItem final : public FormatItem
    {
    public:
        void format(fmt::memory_buffer &buffer, const LogMessage &msg) override
        {
            (void)msg; // 避免未使用参数警告
            fmt::format_to(std::back_inserter(buffer), "{}", "\t");
        }
    };

    /**
     * @brief 换行符格式化项
     * 在日志中添加换行符
     */
    class NLineFormatItem final : public FormatItem
    {
    public:
        void format(fmt::memory_buffer &buffer, const LogMessage &msg) override
        {
            (void)msg; // 避免未使用参数警告
            fmt::format_to(std::back_inserter(buffer), "{}", "\n");
        }
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
        explicit OtherFormatItem(std::string str)
            : str_(std::move(str))
        {
        }

        void format(fmt::memory_buffer &buffer, const LogMessage &msg) override
        {
            (void)msg; // 避免未使用参数警告
            fmt::format_to(std::back_inserter(buffer), "{}", str_);
        }

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
        explicit Formatter(std::string pattern = "[%d{%H:%M:%S}][%t][%c][%f:%l][%p]%T%m%n")
            : pattern_(std::move(pattern))
        {
            if (!parsePattern())
            {
                ;
            }
        }

        /**
         * @brief 格式化日志消息
         * @param buffer 输出缓冲区
         * @param msg 日志消息
         */
        void format(fmt::memory_buffer &buffer, const LogMessage &msg) const
        {
            for (auto &item : items_)
            {
                item->format(buffer, msg);
            }
        }

    protected:
        /**
         * @brief 解析格式化字符串
         * @return 解析成功返回true，否则返回false
         */
        bool parsePattern()
        {
            std::vector<std::pair<std::string, std::string>> fmt_order;
            size_t pos = 0;
            std::string key, val;
            const size_t n = pattern_.size();
            while (pos < n)
            {
                // 1. 不是%字符
                if (pattern_[pos] != '%')
                {
                    val.push_back(pattern_[pos++]);
                    continue;
                }

                // 2.是%%--转换为%字符
                if (pos + 1 < n && pattern_[pos + 1] == '%')
                {
                    val.push_back('%');
                    pos += 2;
                    continue;
                }

                // 3. 如果起始不是%添加
                if (!val.empty())
                {
                    fmt_order.emplace_back("", val);
                    val.clear();
                }

                // 4. 是%，开始处理格式化字符
                if (++pos == n)
                {
                    std::cerr << "%之后没有格式化字符" << std::endl;
                    return false;
                }

                key = pattern_[pos];
                pos++;

                // 5. 处理子规则字符
                if (pos < n && pattern_[pos] == '{')
                {
                    pos++;
                    while (pos < n && pattern_[pos] != '}')
                    {
                        val.push_back(pattern_[pos++]);
                    }

                    if (pos == n)
                    {
                        std::cerr << "未找到匹配的子规则字符 }" << std::endl;
                        return false;
                    }
                    pos++;
                }

                // 6. 插入对应的key与value
                fmt_order.emplace_back(key, val);

                key.clear();
                val.clear();
            }

            // 7. 添加对应的格式化子对象
            for (auto &item : fmt_order)
            {
                items_.push_back(createItem(item.first, item.second));
            }

            return true;
        }

        /**
         * @brief 根据格式化字符创建对应的格式化项
         * @param key 格式化字符
         * @param val 格式化参数
         * @return 格式化项智能指针
         */
        static FormatItem::prt createItem(const std::string &key, const std::string &val)
        {

            if (key == "d")
            {
                if (!val.empty())
                {
                    return std::make_shared<TimeFormatItem>(val);
                }
                else
                {
                    // 单独%d采用默认格式输出
                    return std::make_shared<TimeFormatItem>();
                }
            }
            else if (key == "t")
                return std::make_shared<ThreadIdFormatItem>();
            else if (key == "c")
                return std::make_shared<LoggerFormatItem>();
            else if (key == "f")
                return std::make_shared<FileFormatItem>();
            else if (key == "l")
                return std::make_shared<LineFormatItem>();
            else if (key == "p")
                return std::make_shared<LevelFormatItem>();
            else if (key == "T")
                return std::make_shared<TabFormatItem>();
            else if (key == "m")
                return std::make_shared<MessageFormatItem>();
            else if (key == "n")
                return std::make_shared<NLineFormatItem>();

            if (!key.empty())
            {
                std::cerr << "没有对应的格式化字符: %" << key << std::endl;
                abort();
            }
            return std::make_shared<OtherFormatItem>(val);
        }

    protected:
        std::string pattern_;                       ///< 格式化字符串
        std::vector<FormatItem::prt> items_;       ///< 格式化项列表
    };
} // namespace zlog

