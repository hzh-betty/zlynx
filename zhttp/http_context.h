#pragma once
#include "zhttp/content/parsed_request_body.h"
#include "zhttp/http_request.h"
#include "zhttp/http_response.h"
#include "zhttp/websocket/websocket_handler.h"
#include <functional>
#include <string>
#include <nlohmann/json.hpp>
namespace zhttp {
struct ConnectionInfo {
    std::string remote_address;
    std::string local_address;
    bool tls = false;
};

class Session;
enum class CompletionResult { Completed, Failed, Cancelled, Upgraded };
class HttpContext {
  public:
    using ptr = std::shared_ptr<HttpContext>;
    using Params = HttpRequest::Params;
    using Json = nlohmann::json;
    explicit HttpContext(std::shared_ptr<const HttpRequest> request,
                         ConnectionInfo info = {});
    ~HttpContext();
    const HttpRequest &request() const { return *request_; }
    std::shared_ptr<const HttpRequest> request_ptr() const { return request_; }
    HttpResponse &response() { return response_; }
    const HttpResponse &response() const { return response_; }
    const ConnectionInfo &connection_info() const { return connection_; }
    HttpMethod method() const { return request().method(); }
    HttpVersion version() const { return request().version(); }
    const std::string &path() const { return request().path(); }
    const std::string &query() const { return request().query(); }
    const std::string &body() const { return request().body(); }
    std::string header(const std::string &key,
                       const std::string &fallback = "") const {
        return request().header(key, fallback);
    }
    std::string content_type() const { return request().content_type(); }
    bool is_keep_alive() const { return request().is_keep_alive(); }
    const std::string &remote_addr() const {
        return connection_.remote_address;
    }
    void set_path_params(Params params) { path_params_ = std::move(params); }
    const Params &path_params() const { return path_params_; }
    std::string path_param(const std::string &key,
                           const std::string &fallback = "") const;
    std::string query_param(const std::string &key,
                            const std::string &fallback = "") const {
        return request().query_param(key, fallback);
    }
    const Params &cookies() const;
    std::string cookie(const std::string &key,
                       const std::string &fallback = "") const;
    std::shared_ptr<Session> session() const { return session_; }
    void set_session(std::shared_ptr<Session> session) {
        session_ = std::move(session);
    }
    bool is_json() const { return ParsedRequestBody::is_json(content_type()); }
    bool parse_json() { return parsed_.parse_json(content_type()); }
    const Json *json() const { return parsed_.json(content_type()); }
    const std::string &json_error() const { return parsed_.json_error(); }
    bool is_form_urlencoded() const {
        return ParsedRequestBody::is_form_urlencoded(content_type());
    }
    bool parse_form_urlencoded() {
        return parsed_.parse_form_urlencoded(content_type());
    }
    const Params &form_params() const {
        return parsed_.form_params(content_type());
    }
    std::string form_param(const std::string &key,
                           const std::string &fallback = "") const;
    bool is_multipart() const {
        return ParsedRequestBody::is_multipart(content_type());
    }
    bool parse_multipart();
    const MultipartFormData *multipart() const;
    const std::string &multipart_error() const {
        return parsed_.multipart_error();
    }
    void upgrade_to_websocket(WebSocketCallbacks callbacks,
                              const WebSocketOptions &options = {});
    const WebSocketUpgrade *upgrade() const { return upgrade_.get(); }
    std::unique_ptr<WebSocketUpgrade> take_upgrade() {
        return std::move(upgrade_);
    }
    void reset_result();
    void on_complete(std::function<void(CompletionResult)> callback);
    void complete(CompletionResult result);
    bool completed() const { return completed_; }

  protected:
    // Context owns the request snapshot; test builders can rebuild before
    // execution.
    void invalidate_derived();
    ConnectionInfo connection_;

  private:
    std::shared_ptr<const HttpRequest> request_;
    HttpResponse response_;
    ParsedRequestBody parsed_;
    Params path_params_;
    mutable Params cookies_;
    mutable bool cookies_parsed_ = false;
    std::shared_ptr<Session> session_;
    std::unique_ptr<WebSocketUpgrade> upgrade_;
    std::vector<std::function<void(CompletionResult)>> completion_callbacks_;
    bool completed_ = false;
};
} // namespace zhttp
