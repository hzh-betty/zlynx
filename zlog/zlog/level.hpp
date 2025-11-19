#pragma once
#include <iostream>
#include <string>

/**
 * @brief 日志等级模块
 * 定义日志等级枚举和相关转换接口
 */

namespace zlog
{
    /**
     * @brief 日志级别类
     */
    class LogLevel
    {
    public:
        /**
         * @brief 日志级别枚举
         */
        enum class value
        {
            UNKNOWN = 0,
            DEBUG,
            INFO,
            WARNING,
            ERROR,
            FATAL,
            OFF,
        };
        
        /**
         * @brief 将日志级别枚举转换为字符串
         * @param level 日志级别
         * @return std::string 对应的字符串
         */
        static std::string toString(const LogLevel::value level)
        {
            switch (level)
            {
            case LogLevel::value::DEBUG:
                return "DEBUG";
            case LogLevel::value::INFO:
                return "INFO";
            case LogLevel::value::WARNING:
                return "WARNING";
            case LogLevel::value::ERROR:
                return "ERROR";
            case LogLevel::value::FATAL:
                return "FATAL";
            case LogLevel::value::OFF:
                return "OFF";
            case LogLevel::value::UNKNOWN:
            default:
                return "UNKNOWN";
            }
        }
    };
} // namespace zlog