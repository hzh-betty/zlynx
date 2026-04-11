#include "zhttp/mid/request_body_middleware.h"

#include "zhttp/http_common.h"

#include <utility>

namespace zhttp {
namespace mid {

namespace {

std::string normalize_mime_type(std::string content_type) {
    const size_t semi = content_type.find(';');
    if (semi != std::string::npos) {
        content_type.resize(semi);
    }
    trim(content_type);
    return to_lower(content_type);
}

} // namespace

RequestBodyMiddleware::RequestBodyMiddleware(
    RequestBodyMiddleware::Options options)
    : options_(std::move(options)) {}

bool RequestBodyMiddleware::before(const HttpRequest::ptr &request,
                                   HttpResponse &response) {
    const std::string mime_type = normalize_mime_type(request->content_type());
    if (mime_type.empty()) {
        return true;
    }

    if (options_.parse_json && mime_type == "application/json") {
        if (!request->parse_json() && options_.reject_invalid_json) {
            response.status(HttpStatus::BAD_REQUEST)
                .content_type("text/plain; charset=utf-8")
                .body(options_.invalid_json_message);
            return false;
        }
    }

    if (options_.parse_form_urlencoded &&
        mime_type == "application/x-www-form-urlencoded") {
        request->parse_form_urlencoded();
    }

    if (options_.parse_multipart && mime_type == "multipart/form-data") {
        if (!request->parse_multipart() && options_.reject_invalid_multipart) {
            response.status(HttpStatus::BAD_REQUEST)
                .content_type("text/plain; charset=utf-8")
                .body(options_.invalid_multipart_message);
            return false;
        }
    }

    return true;
}

void RequestBodyMiddleware::after(const HttpRequest::ptr &, HttpResponse &) {}

} // namespace mid
} // namespace zhttp
