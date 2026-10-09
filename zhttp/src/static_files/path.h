/**
 * path.h
 * 静态资源路径映射与相对路径校验。
 *
 * @author hzh-betty
 */

#ifndef ZHTTP_STATIC_FILES_PATH_H_
#define ZHTTP_STATIC_FILES_PATH_H_

#include <string>

namespace zhttp {

/**
 * 路径相关公共操作集合
 *
 * 该模块负责“HTTP 路径到磁盘路径”这条链路上的通用逻辑，重点是：
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
namespace static_path {
/**
 * 规范化 URI 前缀
 *
 * 规则：
 * - 空串与 "/" 都归一为 "/"；
 * - 自动补齐前导 "/"；
 * - 去掉尾随多余 "/"（根前缀除外）。
 *
 * @param prefix 原始配置前缀（例如 "assets"、"/assets/"、"/"）
 * @return 规范化后的前缀
 */
std::string normalize_prefix(const std::string &prefix);

/**
 * 判断请求路径是否应由指定前缀接管
 *
 * 匹配保证“目录边界安全”：
 * - /assets 可命中 /assets 与 /assets/app.js；
 * - /assets 不会误命中 /assets2。
 *
 * @param path 请求路径（不含 query）
 * @param normalized_prefix 已规范化的前缀
 * @return true 表示命中前缀
 */
bool should_handle_path(const std::string &path,
                        const std::string &normalized_prefix);

/**
 * 将请求路径映射为相对路径
 *
 * 例如：
 * - prefix=/static, path=/static/js/app.js => /js/app.js
 * - prefix=/static, path=/static          => /
 *
 * @param path 请求路径
 * @param normalized_prefix 已规范化的前缀
 * @return 去除前缀后的路径片段（可能以 / 开头）
 */
std::string map_to_relative_path(const std::string &path,
                                 const std::string &normalized_prefix);

/**
 * 清洗并校验相对路径，阻断目录穿越
 *
 * 处理流程：
 * 1) 先 URL 解码；
 * 2) 按 / 分段；
 * 3) 忽略空段与 .；
 * 4) 遇到 .. 直接失败。
 *
 * @param raw 原始路径片段（可能包含 URL 编码）
 * @param out 清洗后的安全相对路径输出（不带前导 /）
 * @return true 表示路径安全；false 表示检测到非法路径
 */
bool sanitize_relative_path(const std::string &raw, std::string &out);

/**
 * 拼接两个路径片段
 *
 * 自动处理边界上的 /，避免出现 // 或漏 /。
 *
 * @param left 左路径
 * @param right 右路径
 * @return 规范拼接后的结果
 */
std::string join_path(const std::string &left, const std::string &right);
} // namespace static_path

} // namespace zhttp
#endif
