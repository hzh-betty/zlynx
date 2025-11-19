#ifndef ZLOG_UTIL_H_
#define ZLOG_UTIL_H_
#include <string>
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
        static time_t getCurrentTime();
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
        static bool exists(const std::string &pathname);

        /**
         * @brief 获取文件所在目录路径
         * @param pathname 文件完整路径
         * @return 目录路径，如果没有找到分隔符则返回"."
         */
        static std::string path(const std::string &pathname);

        /**
         * @brief 递归创建目录
         * @param pathname 要创建的目录路径
         */
        static void createDirectory(const std::string &pathname);

    private:
        /**
         * @brief 创建单个目录
         * @param pathname 目录路径
         */
        static void makeDir(const std::string &pathname);
    };
} // namespace zlog

#endif // ZLOG_UTIL_H_
