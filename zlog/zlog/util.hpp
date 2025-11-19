#pragma once
#include <iostream>
#include <string>
#include <chrono>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace zlog
{
    /**
     * @brief 实用工具模块
     * 提供日期和文件操作的常用接口
     */

    /**
     * @brief 日期工具类
     * 提供时间相关的操作接口
     */
    class Date
    {
    public:
        /**
         * @brief 获取当前系统时间
         * @return 当前时间的时间戳
         */
        static time_t getCurrentTime()
        {
            const auto now = std::chrono::system_clock::now();
            const auto time = std::chrono::system_clock::to_time_t(now);
            return time;
        }
    };

    /**
     * @brief 文件工具类
     * 提供文件和目录操作的接口
     */
    class File
    {
    public:
        /**
         * @brief 判断文件是否存在
         * @param pathname 文件路径
         * @return 文件存在返回true，否则返回false
         */
        static bool exists(const std::string &pathname)
        {
            struct stat st{};
            if (stat(pathname.c_str(), &st) < 0)
            {
                return false;
            }
            return true;
        }

        /**
         * @brief 获取文件所在目录路径
         * @param pathname 文件完整路径
         * @return 目录路径，如果没有找到分隔符则返回"."
         */
        static std::string path(const std::string &pathname)
        {
            const size_t pos = pathname.find_last_of("/\\");
            if (pos == std::string::npos)
                return ".";
            return pathname.substr(0, pos + 1);
        }

        /**
         * @brief 递归创建目录
         * @param pathname 要创建的目录路径
         */
        static void createDirectory(const std::string &pathname)
        {
            // 循环创建目录(mkdir)
            // ./abc/bcd/efg
            size_t pos = 0;
            size_t index = 0;
            while (index < pathname.size())
            {
                pos = pathname.find_first_of("/\\", index);
                if (pos == std::string::npos)
                {
                    // 应该创建完整路径而不是原始路径
                    if (!exists(pathname))
                    {
                        makeDir(pathname);
                    }
                    break;
                }
                std::string parentPath = pathname.substr(0, pos + 1);
                if (!exists(parentPath))
                {
                    // 应该创建父路径而不是原始路径
                    makeDir(parentPath);
                }
                index = pos + 1;
            }
        }

    private:
        /**
         * @brief 创建单个目录
         * @param pathname 目录路径
         */
        static void makeDir(const std::string &pathname)
        {
#ifdef _WIN32
            _mkdir(pathname.c_str());
#else
            mkdir(pathname.c_str(), 0777);
#endif
        }
    };
} // namespace zlog