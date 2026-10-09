#ifndef ZHTTP_WRITER_HTTP_RESPONSE_WRITER_H_
#define ZHTTP_WRITER_HTTP_RESPONSE_WRITER_H_

#include "zhttp/http_context.h"
namespace znet {
class Connection;
}
namespace zhttp {
enum class WriteResult { Completed, Failed };
/** 在连接处理流程中提交响应并同步发送内存、文件或流正文。 */
namespace HttpResponseWriter {
/**
 * 在当前连接回调中同步写出响应，并在发头部前提交响应。
 *
 * @param connection 目标连接。
 * @param context 当前请求及响应。
 * @param upgrade 是否使用升级响应规则。
 * @return 全部数据写出返回 Completed；流长度不符或网络失败返回 Failed。
 * @throws std::logic_error 响应已提交。
 * @throws std::runtime_error 文件范围在提交前失效。
 */
WriteResult send(const std::shared_ptr<znet::Connection> &connection,
                 HttpContext &context, bool upgrade = false);
} // namespace HttpResponseWriter
} // namespace zhttp

#endif
