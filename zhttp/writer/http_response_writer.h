#pragma once
#include "zhttp/http_context.h"
namespace znet {
class TcpConnection;
}
namespace zhttp {
struct ResponsePlan {
    bool send_body = false, chunked = false, close = false;
    std::uint64_t length = HttpBody::UnknownLength;
};
enum class WriteResult { Completed, Failed };
// 写出不再持有异步生命周期，framing 与同步发送集中在一个内部模块。
namespace HttpResponseWriter {
ResponsePlan plan(const HttpRequestLine &request, const HttpResponse &response,
                  bool upgrade = false);
std::string headers(const HttpRequestLine &request, const HttpResponse &response,
                    bool upgrade = false);
std::string serialize(const HttpResponse &response, bool include_body = true,
                      HttpMethod method = HttpMethod::GET);
void serialize_to(const HttpResponse &response, std::string *out,
                  bool include_body = true,
                  HttpMethod method = HttpMethod::GET);
WriteResult send(const std::shared_ptr<znet::TcpConnection> &connection,
                 HttpContext &context, bool upgrade = false);
} // namespace HttpResponseWriter
bool send_all_or_fail(const std::shared_ptr<znet::TcpConnection> &connection,
                      const char *data, std::size_t size);
} // namespace zhttp
