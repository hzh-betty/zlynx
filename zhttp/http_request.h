#pragma once
#include "zhttp/http_body.h"
#include "zhttp/http_common.h"
#include "zhttp/http_headers.h"
#include "zhttp/uri.h"
#include <memory>
#include <unordered_map>
namespace zhttp {
// 请求行作为完整协议值持有；方法、原始目标与版本不会在外层重复保存。
struct HttpRequestLine {
    HttpMethod method = HttpMethod::UNKNOWN;
    Uri target;
    HttpVersion version = HttpVersion::HTTP_1_1;
};

class HttpRequest {
  public:
    using ptr = std::shared_ptr<HttpRequest>;
    using Params = std::unordered_map<std::string, std::string>;
    using Headers = HttpHeaders;
    const HttpRequestLine &request_line() const { return line_; }
    HttpMethod method() const { return line_.method; }
    HttpVersion version() const { return line_.version; }
    const Uri &uri() const { return line_.target; }
    const std::string &path() const { return uri().path(); }
    const std::string &query() const { return uri().query(); }
    const Headers &headers() const { return headers_; }
    const Headers &trailers() const { return trailers_; }
    const std::string &body() const { return body_.content(); }
    const HttpBody &body_source() const { return body_; }
    std::string header(const std::string &key,
                       const std::string &fallback = "") const {
        return headers_.get(key, fallback);
    }
    std::string query_param(const std::string &key,
                            const std::string &fallback = "") const {
        return uri().query_param(key, fallback);
    }
    std::vector<std::string> query_values(const std::string &key) const {
        return uri().query_values(key);
    }
    bool is_keep_alive() const;
    std::size_t content_length() const;
    std::string content_type() const { return header("Content-Type"); }
    // Construction API; business code receives this model through a const
    // Context view.
    void set_method(HttpMethod method) { line_.method = method; }
    void set_version(HttpVersion version) { line_.version = version; }
    void set_target(const std::string &target) { line_.target = Uri(target); }
    void set_path(const std::string &path) {
        set_target(path + (query().empty() ? "" : "?" + query()));
    }
    void set_query(const std::string &query) {
        set_target(path() + (query.empty() ? "" : "?" + query));
    }
    void set_header(const std::string &key, const std::string &value) {
        headers_.set(key, value);
    }
    void append_header(const std::string &key, const std::string &value) {
        headers_.append(key, value);
    }
    void append_trailer(const std::string &key, const std::string &value) {
        trailers_.append(key, value);
    }
    void set_body(std::string body) {
        body_ = HttpBody::memory(std::move(body));
    }

  private:
    HttpRequestLine line_;
    Headers headers_, trailers_;
    HttpBody body_;
};
} // namespace zhttp
