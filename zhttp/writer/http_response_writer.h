#ifndef ZHTTP_WRITER_HTTP_RESPONSE_WRITER_H_
#define ZHTTP_WRITER_HTTP_RESPONSE_WRITER_H_

#include "zhttp/http_context.h"
namespace znet {
class TcpConnection;
}
namespace zhttp {
/** 响应写出计划：是否发送正文、分块编码、关闭连接以及正文长度。 */
struct ResponsePlan {
    bool send_body = false, chunked = false, close = false;
    std::uint64_t length = HttpBody::UnknownLength;
};
/** 同步写出结果，不代表客户端已经消费数据。 */
enum class WriteResult { Completed, Failed };
// 写出不再持有异步生命周期，framing 与同步发送集中在一个内部模块。
/** 集中决定响应边界并同步发送正文，避免头部与正文策略不一致。 */
namespace HttpResponseWriter {
/**
 * 根据请求方法、响应版本和正文类型生成写出计划。
 *
 * @param request 请求行，用于 HEAD 正文抑制。
 * @param response 尚未写出的响应。
 * @param upgrade 是否为有效协议升级，允许最终状态 101。
 * @return 正文长度、编码和连接关闭策略。
 * @throws std::invalid_argument 状态行非法或最终响应为不允许的 1xx。
 */
ResponsePlan plan(const HttpRequestLine &request, const HttpResponse &response,
                  bool upgrade = false);
/** 生成状态行和响应头，统一重算 Content-Length/Transfer-Encoding/Connection。 */
std::string headers(const HttpRequestLine &request, const HttpResponse &response,
                    bool upgrade = false);
/** 序列化响应头及可选内存正文；不会读取文件或执行流回调。 */
std::string serialize(const HttpResponse &response, bool include_body = true,
                      HttpMethod method = HttpMethod::GET);
/** 将序列化结果覆盖到 out；out 为空指针时不执行操作。 */
void serialize_to(const HttpResponse &response, std::string *out,
                  bool include_body = true,
                  HttpMethod method = HttpMethod::GET);
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
WriteResult send(const std::shared_ptr<znet::TcpConnection> &connection,
                 HttpContext &context, bool upgrade = false);
} // namespace HttpResponseWriter
/** 分批发送并排空输出缓冲区；连接无效、缓冲区积压或发送失败返回 false。 */
bool send_all_or_fail(const std::shared_ptr<znet::TcpConnection> &connection,
                      const char *data, std::size_t size);
} // namespace zhttp

#endif // ZHTTP_WRITER_HTTP_RESPONSE_WRITER_H_
