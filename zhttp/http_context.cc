#include "zhttp/http_context.h"
#include "zhttp/content/multipart.h"
#include <stdexcept>
namespace zhttp {
namespace {
std::shared_ptr<const HttpRequest>
require_request(std::shared_ptr<const HttpRequest> request) {
    if (!request)
        throw std::invalid_argument("Missing HTTP request");
    return request;
}
static void parse_cookie_header(const std::string &cookie_header,
                                HttpContext::Params &out) {
    // 每次解析前清空输出，避免重复调用时残留旧数据。
    out.clear();
    size_t pos = 0;
    while (pos < cookie_header.size()) {
        // 一个 cookie 项通常由分号分隔。
        size_t end = cookie_header.find(';', pos);
        if (end == std::string::npos) {
            end = cookie_header.size();
        }

        std::string token = cookie_header.substr(pos, end - pos);
        trim(token);
        if (!token.empty()) {
            // key=value 是最常见格式；某些场景下也可能只有 key。
            size_t eq = token.find('=');
            if (eq != std::string::npos) {
                std::string key = token.substr(0, eq);
                std::string value = token.substr(eq + 1);
                trim(key);
                trim(value);
                if (!key.empty()) {
                    out[key] = value;
                }
            } else {
                // 没有等号时仍然保留这个标记，值置空。
                out[token] = "";
            }
        }

        // 跳到下一个 token 的起始位置。
        pos = end + 1;
    }
}

} // namespace
HttpContext::HttpContext(std::shared_ptr<const HttpRequest> request,
                         ConnectionInfo info)
    : connection_(std::move(info)),
      request_(require_request(std::move(request))), parsed_(request_->body()) {
    response_.set_version(request_->version());
    response_.set_keep_alive(request_->is_keep_alive());
}
HttpContext::~HttpContext() { complete(CompletionResult::Cancelled); }
void HttpContext::invalidate_derived() {
    parsed_.invalidate();
    cookies_parsed_ = false;
    cookies_.clear();
}
const HttpContext::Params &HttpContext::cookies() const {
    if (!cookies_parsed_) {
        cookies_parsed_ = true;
        parse_cookie_header(header("Cookie"), cookies_);
    }
    return cookies_;
}
std::string HttpContext::cookie(const std::string &key,
                                const std::string &fallback) const {
    auto it = cookies().find(key);
    return it == cookies().end() ? fallback : it->second;
}
std::string HttpContext::path_param(const std::string &key,
                                    const std::string &fallback) const {
    auto it = path_params_.find(key);
    return it == path_params_.end() ? fallback : it->second;
}
std::string HttpContext::form_param(const std::string &key,
                                    const std::string &fallback) const {
    auto it = form_params().find(key);
    return it == form_params().end() ? fallback : it->second;
}
bool HttpContext::parse_multipart() {
    return parsed_.parse_multipart(content_type(), [this](std::string *error) {
        return MultipartFormData::parse(request(), error);
    });
}
const MultipartFormData *HttpContext::multipart() const {
    return parsed_.multipart(content_type(), [this](std::string *error) {
        return MultipartFormData::parse(request(), error);
    });
}
void HttpContext::upgrade_to_websocket(WebSocketCallbacks callbacks,
                                       const WebSocketOptions &options) {
    if (response_.committed())
        throw std::logic_error("Response already committed");
    response_.enable_chunked(false).body("").status(
        HttpStatus::SWITCHING_PROTOCOLS);
    response_.set_keep_alive(true);
    upgrade_.reset(new WebSocketUpgrade{std::move(callbacks), options});
}
void HttpContext::reset_result() {
    upgrade_.reset();
    response_ = HttpResponse();
    response_.set_version(request().version());
    response_.set_keep_alive(false);
}
void HttpContext::on_complete(std::function<void(CompletionResult)> callback) {
    if (completed_)
        throw std::logic_error("Request already complete");
    if (callback)
        completion_callbacks_.push_back(std::move(callback));
}
void HttpContext::complete(CompletionResult result) {
    if (completed_)
        return;
    completed_ = true;
    auto callbacks = std::move(completion_callbacks_);
    completion_callbacks_.clear();
    for (auto &callback : callbacks) {
        try {
            callback(result);
        } catch (...) {
        }
    }
}
} // namespace zhttp
