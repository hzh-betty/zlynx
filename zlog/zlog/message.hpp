#pragma once
#include "level.hpp"
#include "util.hpp"
#include <thread>

/**
 * @brief 日志消息模块
 * 定义日志消息的数据结构，包含日志的所有必要信息
 */
namespace zlog
{
    using threadId = std::thread::id;

    /**
     * @brief 日志消息结构体
     * 包含一条日志记录的所有信息
     */
    struct LogMessage
    {
        time_t curtime_;            ///< 日志输出时间
        LogLevel::value level_;     ///< 日志等级
        const char *file_;          ///< 源码文件名称
        size_t line_;               ///< 源码行号
        threadId tid_;              ///< 线程ID
        const char *payload_;       ///< 日志主体消息
        const char *loggerName_;    ///< 日志器名称

        /**
         * @brief 构造函数
         * @param level 日志等级
         * @param file 源码文件名
         * @param line 源码行号
         * @param payload 日志内容
         * @param loggerName 日志器名称
         */
        LogMessage(const LogLevel::value level,
                   const char *file, const size_t line,
                   const char *payload, const char *loggerName)
            : curtime_(Date::getCurrentTime()), level_(level),
              file_(file), line_(line), tid_(std::this_thread::get_id()),
              payload_(payload), loggerName_(loggerName)
        {
        }
    };
} // namespace zlog