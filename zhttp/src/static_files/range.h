/**
 * @file range_parse.h
 *
 * @author hzh-betty
 */
#ifndef ZHTTP_INTERNAL_RANGE_PARSE_H_
#define ZHTTP_INTERNAL_RANGE_PARSE_H_

#include "zhttp/http_context.h"
#include "zhttp/http_response.h"

#include <cstddef>
#include <string>

namespace zhttp {

/**
 * @brief Range 请求解析状态
 *
 * 用于把 Range 头部解析结果显式传递给上层流程，便于统一分支处理：
 * - NONE：没有可处理的 Range，或因 If-Range 不满足而主动忽略；
 * - INVALID：Range 语法非法（当前策略：忽略并回落整包）；
 * - NOT_SATISFIABLE：语义不可满足，应返回 416；
 * - SATISFIABLE：成功解析单范围，应返回 206。
 */
enum class RangeParseState {
    NONE,
    INVALID,
    NOT_SATISFIABLE,
    SATISFIABLE,
};

/**
 * @brief Range 解析结果载体
 *
 * 该结构体用于把“Range 头部解析结论”从解析阶段传递到响应构造阶段。
 *
 * 使用约定：
 * - `state == SATISFIABLE`：
 * - `start` 与 `end` 有效，且表示闭区间 `[start, end]`；
 * - 语义上应满足 `start <= end` 且 `end < content_length`。
 * - `state != SATISFIABLE`：
 * - `start/end` 视为未定义辅助值，调用方不应依赖它们。
 *
 * 典型流程：
 * 1) `parse_range_request(...)` 生成 `ParsedRange`；
 * 2) 静态文件中间件根据 state 构造 200/206/416 响应。
 */
struct ParsedRange {
    /**
     * @brief 解析结果状态，决定响应分支（200/206/416）。
     */
    RangeParseState state = RangeParseState::NONE;

    /**
     * @brief 范围起始字节下标（仅在 SATISFIABLE 时有效）。
     */
    size_t start = 0;

    /**
     * @brief 范围结束字节下标（仅在 SATISFIABLE 时有效，包含该字节）。
     */
    size_t end = 0;
};

/**
 * @brief 解析请求中的单个字节范围
 *
 * 当前实现支持：
 * - bytes=start-end
 * - bytes=start-
 * - bytes=-suffix
 *
 * 当前实现不支持多范围（逗号分隔），遇到则返回 INVALID。
 *
 * @param request HTTP 请求
 * @param content_length 实体总长度（字节）
 * @param last_modified 当前实体 Last-Modified 字符串（用于 If-Range）
 * @return ParsedRange 解析结果
 */
ParsedRange parse_range_request(const HttpContext &request,
                                size_t content_length,
                                const std::string &last_modified);

} // namespace zhttp

#endif
