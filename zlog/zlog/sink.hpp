#pragma once
#include "util.hpp"
#include <fmt/core.h>
#include <fmt/ostream.h>
#include <fmt/os.h>
#include <string>
#include <fstream>
#include <utility>

// 平台相关
#ifdef _WIN32
#include <Windows.h>
#endif

/**
 * @brief 日志落地模块
 * 实现日志输出到不同目标（控制台、文件、滚动文件）
 * 使用工厂模式进行创建与表示的分离
 */
namespace zlog
{
    /**
     * @brief 日志落地抽象基类
     * 定义了日志输出的通用接口
     */
    class LogSink
    {
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
    class StdOutSink final : public LogSink
    {
    public:
        void log(const char *data, size_t len) override
        {
            fmt::print(stdout, "{:.{}}", data, len);
        }
    };

    /**
     * @brief 文件日志落地器
     * 将日志输出到指定文件
     */
    class FileSink final : public LogSink
    {
    public:
        /**
         * @brief 构造函数
         * @param pathname 文件路径
         */
        explicit FileSink(std::string pathname)
            : pathname_(std::move(pathname))
        {
            File::createDirectory(File::path(pathname_));
            ofs_.open(pathname_, std::ios::binary | std::ios::app);
        }

        void log(const char *data, size_t len) override
        {
            fmt::print(ofs_, "{:.{}}", data, len);
            ofs_.flush(); // 确保日志及时写入磁盘
        }

    protected:
        std::string pathname_;      ///< 文件路径
        std::ofstream ofs_;         ///< 输出文件流
    };

    /**
     * @brief 按大小滚动的文件日志落地器
     * 当文件大小超过限制时自动创建新文件
     */
    class RollBySizeSink final : public LogSink
    {
    public:
        /**
         * @brief 构造函数
         * @param basename 文件基础名称
         * @param maxSize 最大文件大小（字节）
         */
        RollBySizeSink(std::string basename, const size_t maxSize)
            : basename_(std::move(basename)),
              maxSize_(maxSize),
              curSize_(0),
              nameCount_(0)
        {
            // 1.创建日志文件所用的路径
            const std::string pathname = createNewFile();
            File::createDirectory(File::path(pathname));

            // 2. 创建并打开日志文件
            ofs_.open(pathname, std::ios::binary | std::ios::app);
        }

        void log(const char *data, size_t len) override
        {
            if (curSize_ + len > maxSize_)
            {
                rollOver();
            }
            fmt::print(ofs_, "{:.{}}", data, len);
            ofs_.flush(); // 确保日志及时写入磁盘
            curSize_ += len;
        }

    protected:
        /**
         * @brief 创建新文件名
         * @return 新文件的完整路径
         */
        std::string createNewFile()
        {
            time_t t = Date::getCurrentTime();
            struct tm lt{};
#ifdef _WIN32
            localtime_s(&lt, &t);
#else
            localtime_r(&t, &lt);
#endif
            // 先将时间格式化为字符串
            char timeStr[64];
            strftime(timeStr, sizeof(timeStr), "%Y%m%d%H%M%S", &lt);
            
            std::string pathname = fmt::format("{}_{}-{}.log",
                                               basename_, timeStr, nameCount_++);

            return pathname;
        }

        /**
         * @brief 滚动到新文件
         * 关闭当前文件，创建新文件
         */
        void rollOver()
        {
            ofs_.close(); // 释放旧流资源
            std::string pathname = createNewFile();
            ofs_.open(pathname, std::ios::binary | std::ios::app);
            curSize_ = 0;
        }

        std::string basename_;      ///< 文件基础名称
        std::ofstream ofs_;         ///< 输出文件流
        size_t maxSize_;            ///< 最大文件大小
        size_t curSize_;            ///< 当前文件大小
        size_t nameCount_;          ///< 文件名计数器
    };

    /**
     * @brief 日志落地器工厂类
     * 使用工厂模式创建不同类型的日志落地器
     */
    class SinkFactory
    {
    public:
        /**
         * @brief 创建日志落地器
         * @tparam SinkType 落地器类型
         * @tparam Args 构造参数类型
         * @param args 构造参数
         * @return 日志落地器智能指针
         */
        template <typename SinkType, typename... Args>
        static LogSink::ptr create(Args &&...args)
        {
            // 使用完美转发构造对象
            return std::make_shared<SinkType>(std::forward<Args>(args)...);
        }
    };
} // namespace zlog