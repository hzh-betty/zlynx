#ifndef ZHTTP_HTTP_REQUEST_H_
#define ZHTTP_HTTP_REQUEST_H_

#include "zhttp/http_common.h"
#include "zhttp/http_headers.h"
#include "zhttp/uri.h"
#include <memory>
#include <unordered_map>

namespace zhttp {
// 请求行作为完整协议值持有；方法、原始目标与版本不会在外层重复保存。
/**
 * @brief 请求行协议值：方法、原始目标和 HTTP 版本。
 */
struct HttpRequestLine {
    HttpMethod method = HttpMethod::UNKNOWN;
    Uri target;
    HttpVersion version = HttpVersion::HTTP_1_1;
};

/**
 * @brief HTTP 请求模型，保存请求行、头部、trailer 和内存正文。
 *
 * set/append 方法供解析器与请求构造使用；业务处理通过 HttpContext
 * 的只读视图访问。 返回的引用与请求对象同寿命，修改请求可能使其内容发生变化。
 */
class HttpRequest {
  public:
    using ptr = std::shared_ptr<HttpRequest>;
    HttpRequest() = default;
    HttpRequest(const HttpRequest &) = delete;
    HttpRequest &operator=(const HttpRequest &) = delete;
    HttpRequest(HttpRequest &&) = default;
    HttpRequest &operator=(HttpRequest &&) = default;
    using Params = std::unordered_map<std::string, std::string>;
    using Headers = HttpHeaders;

    const HttpRequestLine &request_line() const { return line_; }

    HttpMethod method() const { return line_.method; }

    HttpVersion version() const { return line_.version; }

    const Uri &uri() const { return line_.target; }

    /**
     * @brief 返回请求路径，不含查询串。
     */
    const std::string &path() const { return uri().path(); }

    /**
     * @brief 返回原始查询串，不含问号。
     */
    const std::string &query() const { return uri().query(); }

    /**
     * @brief 返回保留重复字段的只读头部集合。
     */
    const Headers &headers() const { return headers_; }

    /**
     * @brief 返回 chunked 正文后的只读 trailer 集合。
     */
    const Headers &trailers() const { return trailers_; }

    const std::string &body() const { return body_; }

    /**
     * @brief 忽略字段名大小写，返回首个值；缺失时返回 fallback。
     */
    std::string header(const std::string &key,
                       const std::string &fallback = "") const {
        return headers_.get(key, fallback);
    }

    /**
     * @brief 借用首个头部值；缺失返回空视图，请求修改或销毁后视图可能失效。
     */
    std::string_view header_view(std::string_view key) const {
        return headers_.get_view(key).value_or("");
    }

    /**
     * @brief 返回指定查询键的首个解码值；缺失时返回 fallback。
     */
    std::string query_param(const std::string &key,
                            const std::string &fallback = "") const {
        return uri().query_param(key, fallback);
    }

    /**
     * @brief 返回指定查询键的所有解码值，保持原顺序。
     */
    std::vector<std::string> query_values(const std::string &key) const {
        return uri().query_values(key);
    }

    /**
     * @brief 依据 HTTP 版本和 Connection 字段判断是否保持连接。
     */
    bool is_keep_alive() const;

    /**
     * @brief 将首个 Content-Length 转为整数；协议合法性由解析器校验。
     */
    std::size_t content_length() const;

    /**
     * @brief 返回首个 Content-Type 值；缺失时返回空串。
     */
    std::string content_type() const { return header("Content-Type"); }

    // 以下接口用于构建请求；业务处理代码通过 Context 的只读视图
    // 访问请求模型。
    void set_method(HttpMethod method) { line_.method = method; }

    void set_version(HttpVersion version) { line_.version = version; }

    /**
     * @brief 重新解析并设置完整请求目标。
     *
     * @param target 请求目标原文。
     * @throws std::invalid_argument 目标形式或编码非法。
     */
    void set_target(std::string target) {
        line_.target = Uri(std::move(target));
    }

    /**
     * @brief 设置路径并保留现有查询串；目标非法时抛出 std::invalid_argument。
     */
    void set_path(const std::string &path) {
        set_target(path + (query().empty() ? "" : "?" + query()));
    }

    /**
     * @brief 设置查询串并保留现有路径；目标非法时抛出 std::invalid_argument。
     */
    void set_query(const std::string &query) {
        set_target(path() + (query.empty() ? "" : "?" + query));
    }

    /**
     * @brief 替换全部同名头部；非法字段抛出 std::invalid_argument。
     */
    void set_header(std::string_view key, std::string_view value) {
        headers_.set(key, value);
    }

    /**
     * @brief 追加头部并保留重复字段；非法字段抛出 std::invalid_argument。
     */
    void append_header(std::string_view key, std::string_view value) {
        headers_.append(key, value);
    }

    /**
     * @brief 追加 trailer；非法字段抛出 std::invalid_argument。
     */
    void append_trailer(std::string_view key, std::string_view value) {
        trailers_.append(key, value);
    }

    void set_body(std::string body) { body_ = std::move(body); }

  private:
    HttpRequestLine line_;
    Headers headers_, trailers_;
    std::string body_;
};
} // namespace zhttp

#endif
