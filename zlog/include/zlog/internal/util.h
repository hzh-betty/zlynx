/**
 * @file util.h
 * @brief util 定义。
 * @author hzh-betty
 */

#ifndef ZLOG_INTERNAL_UTIL_H_
#define ZLOG_INTERNAL_UTIL_H_

#include <sys/stat.h>

#include <string>

namespace zlog {

/**
 * @brief 文件工具类
 * 提供文件和目录操作的接口
 */
class File {
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
    static void create_directory(const std::string &pathname);

  private:
    /**
     * @brief 创建单个目录
     * @param pathname 目录路径
     */
    static void make_dir(const std::string &pathname);
};
} // namespace zlog

#endif // ZLOG_INTERNAL_UTIL_H_
