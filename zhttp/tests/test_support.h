#ifndef ZHTTP_TESTS_TEST_SUPPORT_H_
#define ZHTTP_TESTS_TEST_SUPPORT_H_

#include "zhttp/http_context.h"
#include "zhttp/http_server.h"
#include "zhttp/parser/http_request_parser.h"
#include "zhttp/parser/websocket_frame_parser.h"
#include "zhttp/pipeline/request_pipeline.h"
#include "zhttp/protocol/websocket_protocol_handler.h"
#include "zhttp/websocket/websocket_handshake.h"
#include "zhttp/websocket/websocket_message_assembler.h"
#include "zhttp/writer/http_response_writer.h"
#include "znet/buffer.h"
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
class TestApplication : public Router {
  public:
    using ExceptionHandler = zhttp::ExceptionHandler;
    void use(mid::Middleware::ptr mw) { pipeline_.use(std::move(mw)); }
    void use(const std::string &path, mid::Middleware::ptr mw) {
        pipeline_.use(path, std::move(mw));
    }
    void use_group(const std::string &path, mid::Middleware::ptr mw) {
        pipeline_.use_group(path, std::move(mw));
    }
    void set_not_found_handler(HttpHandler handler) {
        pipeline_.set_not_found_handler(std::move(handler));
    }
    void set_not_found_handler(RouteHandler::ptr handler) {
        set_not_found_handler(make_route_callback(std::move(handler)));
    }
    void set_exception_handler(ExceptionHandler handler) {
        pipeline_.set_exception_handler(std::move(handler));
    }
    const ExceptionHandler &exception_handler() const {
        return pipeline_.exception_handler();
    }
    bool route(const TestContext::ptr &context, HttpResponse &response) {
        context->response() = std::move(response);
        const bool found = pipeline_.execute(*context, *this);
        response = std::move(context->response());
        return found;
    }

  private:
    RequestPipeline pipeline_;
};
inline bool run_middleware(mid::Middleware &middleware,
                           const TestContext::ptr &context,
                           HttpResponse &response) {
    context->response() = std::move(response);
    const bool proceed = middleware.before(*context);
    middleware.after(*context);
    response = std::move(context->response());
    return proceed;
}
inline bool run_server(HttpServer &server, const TestContext::ptr &context,
                       HttpResponse &response) {
    context->response() = std::move(response);
    bool found = server.handle(*context);
    response = std::move(context->response());
    return found;
}
inline ParseResult parse_request(HttpRequestParser &parser,
                                 znet::Buffer *buffer) {
    auto result = parser.parse(buffer);
    return result == ParseResult::HEADERS_READY ? parser.parse(buffer) : result;
}
// Existing frame cases exercise both the newly separated parser and assembler.
class TestWebSocketParser {
  public:
    explicit TestWebSocketParser(size_t limit = kDefaultWebSocketMaxMessageSize)
        : max_message_size_(limit), assembler_(limit) {}
    bool parse(znet::Buffer *buffer, std::vector<WebSocketFrameEvent> *events,
               uint16_t *code, std::string *error) {
        if (!buffer || !events || !code || !error)
            return false;
        events->clear();
        while (buffer->readable_bytes()) {
            std::vector<WebSocketFrameEvent> frames;
            if (!parse_websocket_frame(buffer, &frames, code, error,
                                       max_message_size_))
                return false;
            if (frames.empty())
                break;
            if (!assembler_.assemble(std::move(frames.front()), *events, *code,
                                     *error))
                return false;
        }
        return true;
    }

  private:
    size_t max_message_size_;
    WebSocketMessageAssembler assembler_;
};
} // namespace zhttp

namespace zhttp {
inline bool build_websocket_handshake_response(
    const std::shared_ptr<const HttpRequest> &request, std::string *output,
    std::string *error, const std::string &selected = "") {
    if (!output) {
        if (error)
            *error = "Response output is null";
        return false;
    }
    if (!error)
        return false;
    HttpResponse response;
    std::string negotiated;
    if (!prepare_websocket_handshake(request, {}, response, negotiated, *error))
        return false;
    if (!selected.empty()) {
        if (!HttpHeaders::valid(selected, "")) {
            *error = "Invalid selected subprotocol";
            return false;
        }
        response.header("Sec-WebSocket-Protocol", selected);
    }
    *output = HttpResponseWriter::serialize(response);
    return true;
}
} // namespace zhttp

#endif // ZHTTP_TESTS_TEST_SUPPORT_H_
