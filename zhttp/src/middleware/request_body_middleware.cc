/**
 * request_body_middleware.cc
 * request_body_middleware 实现。
 *
 * @author hzh-betty
 */

#include "zhttp/middleware/request_body_middleware.h"

#include "zhttp/http_common.h"

#include <utility>

namespace zhttp {
namespace mid {

RequestBodyMiddleware::RequestBodyMiddleware(
    RequestBodyMiddleware::Options options)
    : options_(std::move(options)) {}

bool RequestBodyMiddleware::before(HttpContext &request) {
    auto &response = request.response();
    const std::string mime_type = normalize_mime_type(request.content_type());
    if (mime_type.empty()) {
        return true;
    }

    if (options_.parse_json && mime_type == "application/json") {
        if (!request.parse_json() && options_.reject_invalid_json) {
            response.status(HttpStatus::BAD_REQUEST)
                .content_type("text/plain; charset=utf-8")
                .body(options_.invalid_json_message);
            return false;
        }
    }

    if (options_.parse_form_urlencoded &&
        mime_type == "application/x-www-form-urlencoded") {
        request.parse_form_urlencoded();
    }

    if (options_.parse_multipart && mime_type == "multipart/form-data") {
        if (!request.parse_multipart() && options_.reject_invalid_multipart) {
            response.status(HttpStatus::BAD_REQUEST)
                .content_type("text/plain; charset=utf-8")
                .body(options_.invalid_multipart_message);
            return false;
        }
    }

    return true;
}

} // namespace mid
} // namespace zhttp
