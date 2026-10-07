/**
 * @file format.h
 * @brief format 定义。
 * @author hzh-betty
 */

#ifndef ZLOG_FORMAT_H_
#define ZLOG_FORMAT_H_
#include <memory>
#include <vector>

#include <fmt/color.h>
#include <fmt/core.h>

#include "zlog/message.h"
namespace zlog {
static const std::string kTimeFormatDefault = "%H:%M:%S"; // 默认时间输出格式

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
class Formatter {
  public:
    using ptr = std::shared_ptr<Formatter>;

    /**
     * @brief 构造函数
     * @param pattern 格式化字符串
     */
    explicit Formatter(
        std::string pattern = "[%d{%H:%M:%S}][%t][%c][%f:%l][%p]%T%m%n");

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
    bool parse_pattern();

    // 格式项是已解析规则的值；key 为 0 时 value 保存普通文本。
    struct Item {
        char key;
        std::string value;
    };
    static Item create_item(const std::string &key, const std::string &val);

    std::string pattern_;     // 格式化字符串
    std::vector<Item> items_; // 格式化项列表
};
} // namespace zlog

#endif // ZLOG_FORMAT_H_
