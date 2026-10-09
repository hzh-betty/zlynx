#include "zhttp/http_application.h"
#include "application/request_pipeline.h"

namespace zhttp {
struct HttpApplication::Impl {
    Router router;
    RequestPipeline pipeline;
};

HttpApplication::HttpApplication() : impl_(std::make_unique<Impl>()) {}
HttpApplication::~HttpApplication() = default;
Router &HttpApplication::router() { return impl_->router; }
const Router &HttpApplication::router() const { return impl_->router; }
void HttpApplication::use(mid::Middleware::ptr middleware) {
    impl_->pipeline.use(std::move(middleware));
}
void HttpApplication::use(const std::string &path, mid::Middleware::ptr middleware) {
    impl_->pipeline.use(path, std::move(middleware));
}
void HttpApplication::use_group(const std::string &prefix,
                                mid::Middleware::ptr middleware) {
    impl_->pipeline.use_group(prefix, std::move(middleware));
}
void HttpApplication::set_not_found_handler(HttpHandler handler) {
    impl_->pipeline.set_not_found_handler(std::move(handler));
}
void HttpApplication::set_exception_handler(ExceptionHandler handler) {
    impl_->pipeline.set_exception_handler(std::move(handler));
}
void HttpApplication::freeze() {
    impl_->router.freeze();
    impl_->pipeline.freeze();
}
bool HttpApplication::handle(HttpContext &context) const {
    return impl_->pipeline.execute(context, impl_->router);
}
} // namespace zhttp
