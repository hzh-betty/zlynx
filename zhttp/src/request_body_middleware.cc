#include "request_body_middleware.h"

#include <utility>

namespace zhttp {

RequestBodyMiddleware::RequestBodyMiddleware(RequestBodyMiddleware::Options options)
    : options_(std::move(options)) {}

bool RequestBodyMiddleware::before(const HttpRequest::ptr &request,
                                   HttpResponse &response) {
  if (options_.parse_json && request->is_json()) {
    if (!request->parse_json() && options_.reject_invalid_json) {
      response.status(HttpStatus::BAD_REQUEST)
          .content_type("text/plain; charset=utf-8")
          .body(options_.invalid_json_message);
      return false;
    }
  }

  if (options_.parse_form_urlencoded && request->is_form_urlencoded()) {
    request->parse_form_urlencoded();
  }

  if (options_.parse_multipart && request->is_multipart()) {
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

} // namespace zhttp
