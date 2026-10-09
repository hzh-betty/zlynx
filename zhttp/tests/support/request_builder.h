#ifndef ZHTTP_TESTS_REQUEST_BUILDER_H_
#define ZHTTP_TESTS_REQUEST_BUILDER_H_
#include "zhttp/http_application.h"
#include <gtest/gtest.h>
#include <stdexcept>
#include <unistd.h>
namespace zhttp {
inline std::string representation_bytes(const HttpResponse &response) {
    const auto &body = response.body_source();
    if (body.kind() != HttpBody::Kind::File)
        return response.body_content();
    const auto *file = body.file_resource();
    std::string bytes(file->size, '\0');
    size_t read = 0;
    while (read < bytes.size()) {
        auto n = ::pread(file->fd, &bytes[read], bytes.size() - read,
                         file->offset + read);
        if (n <= 0)
            throw std::runtime_error("Cannot read test file body");
        read += n;
    }
    return bytes;
}
// Mutable builders are confined to tests; handlers receive the const protocol
// view.
class TestContext : public HttpContext {
  public:
    using ptr = std::shared_ptr<TestContext>;
    TestContext() : TestContext(std::make_shared<HttpRequest>()) {}
    void set_method(HttpMethod method) { model_->set_method(method); }
    void set_version(HttpVersion version) { model_->set_version(version); }
    void set_path(const std::string &path) { model_->set_path(path); }
    void set_query(const std::string &query) { model_->set_query(query); }
    void set_header(const std::string &name, const std::string &value) {
        model_->set_header(name, value);
        invalidate_derived();
    }
    void set_body(std::string body) {
        model_->set_body(std::move(body));
        invalidate_derived();
    }
    void set_remote_addr(std::string addr) {
        connection_.remote_address = std::move(addr);
    }
    void set_path_param(const std::string &key, const std::string &value) {
        auto params = path_params();
        params[key] = value;
        set_path_params(std::move(params));
    }
    const HttpHeaders &headers() const { return request().headers(); }
    size_t content_length() const { return request().content_length(); }
    void parse_query_params() {}
    Params query_params() const {
        Params result;
        parse_urlencoded_params(query(), result);
        return result;
    }

  private:
    explicit TestContext(HttpRequest::ptr model)
        : HttpContext(model), model_(std::move(model)) {}
    HttpRequest::ptr model_;
};
inline bool run_application(const HttpApplication &application,
                            const TestContext::ptr &context,
                            HttpResponse &response) {
    context->response() = std::move(response);
    const bool found = application.handle(*context);
    response = std::move(context->response());
    return found;
}
inline bool run_middleware(mid::Middleware &middleware,
                           const TestContext::ptr &context,
                           HttpResponse &response) {
    context->response() = std::move(response);
    const bool proceed = middleware.before(*context);
    middleware.after(*context);
    response = std::move(context->response());
    return proceed;
}
} // namespace zhttp
#endif
