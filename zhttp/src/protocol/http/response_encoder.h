#ifndef ZHTTP_WRITER_RESPONSE_ENCODER_H_
#define ZHTTP_WRITER_RESPONSE_ENCODER_H_
#include "zhttp/http_request.h"
#include "zhttp/http_response.h"
namespace zhttp {
struct ResponsePlan {
    bool send_body = false, chunked = false, close = false;
    std::uint64_t length = HttpBody::UnknownLength;
};
/** 纯 HTTP framing 与编码；不提交响应、不访问文件或网络、不执行流回调。 */
namespace ResponseEncoder {
ResponsePlan plan(const HttpRequestLine &request, const HttpResponse &response, bool upgrade = false);
std::string headers(const HttpRequestLine &request, const HttpResponse &response, bool upgrade = false);
std::string chunk(const char *data, size_t size);
std::string serialize(const HttpResponse &response, bool include_body = true,
                      HttpMethod method = HttpMethod::GET);
void serialize_to(const HttpResponse &response, std::string *out, bool include_body = true,
                  HttpMethod method = HttpMethod::GET);
}
}
#endif
