#ifndef ZHTTP_WRITER_RESPONSE_ENCODER_H_
#define ZHTTP_WRITER_RESPONSE_ENCODER_H_
#include "zhttp/http_request.h"
#include "zhttp/http_response.h"

namespace zhttp {
/**
 * @brief 从请求方法与响应正文推导的写出计划，不拥有或发送正文。
 *
 * length 描述实体长度，UnknownLength 表示未知；HEAD 可保留长度却不发送正文。
 * 计划生成后若修改响应，须重新计算，避免头部与实际正文边界不一致。
 */
struct ResponsePlan {
    bool send_body = false, chunked = false, close = false;
    std::uint64_t length = HttpBody::UnknownLength;
};

/**
 * @brief 纯 HTTP framing 与编码；不提交响应、不访问文件或网络、不执行流回调。
 */
namespace ResponseEncoder {
/**
 * @brief 校验最终状态行并确定正文、分块编码和连接关闭策略。
 *
 * @param upgrade 允许 101 作为最终响应；普通 1xx 不能作为最终响应。
 * @throws std::invalid_argument 状态码、响应版本或最终状态不受支持。
 */
ResponsePlan plan(const HttpRequestLine &request, const HttpResponse &response,
                  bool upgrade = false);
std::string headers(const HttpRequestLine &request,
                    const HttpResponse &response, bool upgrade = false);
std::string headers(const HttpResponse &response, const ResponsePlan &plan,
                    bool upgrade = false);

/**
 * @brief 将非空字节串封装为一个 HTTP chunk，保留内嵌 NUL。
 * @pre size 非零时 data 指向至少 size 个可读字节。
 * @return size 为 0 时返回空串，不生成终止 chunk；终止标记由调用方追加。
 */
std::string chunk(const char *data, size_t size);

/**
 * @brief 生成响应头及可选内存正文，不读取文件或调用流回调。
 *
 * @param include_body 是否包含允许发送的内存正文，不改变头部中描述的长度。
 * @param method 用于判断 HEAD 等正文抑制规则。
 * @return 文件或流正文只生成头部；不能据此认定完整正文已经序列化。
 */
std::string serialize(const HttpResponse &response, bool include_body = true,
                      HttpMethod method = HttpMethod::GET);

/**
 * @brief 将 serialize() 结果覆盖写入 out；out 为空时无操作。
 */
void serialize_to(const HttpResponse &response, std::string *out,
                  bool include_body = true,
                  HttpMethod method = HttpMethod::GET);
} // namespace ResponseEncoder
} // namespace zhttp
#endif
