#ifndef ZHTTP_INTERNAL_HTTP_UTILS_H_
#define ZHTTP_INTERNAL_HTTP_UTILS_H_

#include <chrono>
#include <cstdint>
#include <ctime>
#include <string>

namespace zhttp {

/**
 * @brief 日期时间公共操作集合
 * @details
 * 统一管理 HTTP 场景中的时间类型与常用时间工具函数。
 */
class TimerHelper {
  public:
    using SteadyClock = std::chrono::steady_clock;
    using SteadyTimePoint = SteadyClock::time_point;
    using Milliseconds = std::chrono::milliseconds;
    using Seconds = std::chrono::seconds;

    /**
     * @brief 将 Unix 时间戳格式化为 HTTP GMT 时间字符串
     * @param timestamp 秒级 Unix 时间戳
     * @return 形如 `Wed, 21 Oct 2015 07:28:00 GMT` 的字符串
     */
    static std::string format_http_date_gmt(std::time_t timestamp);

    static SteadyTimePoint steady_now();

    static Milliseconds milliseconds(int64_t value);

    static Seconds seconds(int64_t value);

    template <typename Rep, typename Period>
    static Milliseconds
    to_milliseconds(const std::chrono::duration<Rep, Period> &duration) {
        return std::chrono::duration_cast<Milliseconds>(duration);
    }
};

/**
 * @brief 路径相关公共操作集合
 * @details
 * 该类负责“HTTP 路径到磁盘路径”这条链路上的通用逻辑，重点是：
 * - 统一前缀格式（避免 /assets、assets/ 混乱）；
 * - 判断请求是否命中某个挂载前缀；
 * - 将 URL 路径映射成相对路径；
 * - 做路径清洗与目录穿越拦截（例如拒绝 ..）。
 *
 * 设计约定：
 * - 所有函数均为纯工具函数，不依赖对象状态；
 * - 失败场景通过返回值表达，不抛异常；
 * - 输入输出都使用 UTF-8 字符串语义，不做平台特定路径分隔符扩展。
 */
class PathOperator {
  public:
    /**
     * @brief 规范化 URI 前缀
     * @param prefix 原始配置前缀（例如 "assets"、"/assets/"、"/"）
     * @return 规范化后的前缀
     * @details
     * 规则：
     * - 空串与 "/" 都归一为 "/"；
     * - 自动补齐前导 "/"；
     * - 去掉尾随多余 "/"（根前缀除外）。
     */
    static std::string normalize_prefix(const std::string &prefix);

    /**
     * @brief 判断请求路径是否应由指定前缀接管
     * @param path 请求路径（不含 query）
     * @param normalized_prefix 已规范化的前缀
     * @return true 表示命中前缀
     * @details
     * 匹配保证“目录边界安全”：
     * - /assets 可命中 /assets 与 /assets/app.js；
     * - /assets 不会误命中 /assets2。
     */
    static bool should_handle_path(const std::string &path,
                                   const std::string &normalized_prefix);

    /**
     * @brief 将请求路径映射为相对路径
     * @param path 请求路径
     * @param normalized_prefix 已规范化的前缀
     * @return 去除前缀后的路径片段（可能以 / 开头）
     * @details
     * 例如：
     * - prefix=/static, path=/static/js/app.js => /js/app.js
     * - prefix=/static, path=/static          => /
     */
    static std::string
    map_to_relative_path(const std::string &path,
                         const std::string &normalized_prefix);

    /**
     * @brief 清洗并校验相对路径，阻断目录穿越
     * @param raw 原始路径片段（可能包含 URL 编码）
     * @param out 清洗后的安全相对路径输出（不带前导 /）
     * @return true 表示路径安全；false 表示检测到非法路径
     * @details
     * 处理流程：
     * 1) 先 URL 解码；
     * 2) 按 / 分段；
     * 3) 忽略空段与 .；
     * 4) 遇到 .. 直接失败。
     */
    static bool sanitize_relative_path(const std::string &raw,
                                       std::string &out);

    /**
     * @brief 拼接两个路径片段
     * @param left 左路径
     * @param right 右路径
     * @return 规范拼接后的结果
     * @details
     * 自动处理边界上的 /，避免出现 // 或漏 /。
     */
    static std::string join_path(const std::string &left,
                                 const std::string &right);
};

/**
 * @brief 文件与元信息公共操作集合
 * @details
 * 用于统一项目中常见文件操作，避免各模块重复实现。典型场景包括：
 * - 静态文件读取与类型识别；
 * - 文件存在性/类型判断；
 * - 基于 stat 的缓存验证信息（Last-Modified/ETag）；
 * - 轻量配置文件（如 pid 文件）读写。
 */
class FileOperator {
  public:
    /**
     * @brief 判断是否为常规文件
     * @param path 路径
     * @return true 表示存在且是 regular file
     */
    static bool is_regular_file(const std::string &path);

    /**
     * @brief 判断是否为目录
     * @param path 路径
     * @return true 表示存在且是目录
     */
    static bool is_directory(const std::string &path);

    /**
     * @brief 以二进制方式读取整个文件
     * @param path 文件路径
     * @param content 读取结果输出
     * @return true 表示成功
     */
    static bool read_file(const std::string &path, std::string &content);

    /**
     * @brief 以二进制方式写文件（覆盖写）
     * @param path 文件路径
     * @param content 待写入内容
     * @return true 表示成功
     */
    static bool write_file_binary(const std::string &path,
                                  const std::string &content);

    /**
     * @brief 根据扩展名推断 MIME 类型
     * @param file_path 文件路径
     * @return MIME 字符串，未知扩展名回退为默认类型
     */
    static std::string detect_content_type(const std::string &file_path);

    /**
     * @brief 获取 Last-Modified 响应头值
     * @param path 文件路径
     * @param last_modified 输出 HTTP GMT 时间字符串
     * @return true 表示成功
     */
    static bool get_last_modified(const std::string &path,
                                  std::string &last_modified);

    /**
     * @brief 基于文件元信息生成弱 ETag
     * @param path 文件路径
     * @param etag 输出 ETag（形如 W/"size-mtime_ns"）
     * @return true 表示成功
     */
    static bool get_etag(const std::string &path, std::string &etag);

    /**
     * @brief 从文本文件读取一个 int
     * @param path 文件路径
     * @param value 读取结果
     * @return true 表示成功
     * @details 常用于 pid 文件等简单状态文件。
     */
    static bool read_int_from_file(const std::string &path, int &value);

    /**
     * @brief 向文本文件写入一个 int
     * @param path 文件路径
     * @param value 待写入整数
     * @return true 表示成功
     */
    static bool write_int_to_file(const std::string &path, int value);
};

} // namespace zhttp

#endif // ZHTTP_INTERNAL_HTTP_UTILS_H_
