#include "zhttp/mid/session_middleware.h"

namespace zhttp {
namespace mid {

SessionMiddleware::SessionMiddleware(SessionManager::ptr manager,
                                     SessionMiddleware::Options opt)
    : manager_(std::move(manager)), options_(std::move(opt)) {}

bool SessionMiddleware::before(const HttpRequest::ptr &request,
                               HttpResponse & /*response*/) {
    if (!manager_) {
        return true;
    }

    std::string sid = request->cookie(options_.cookie_name);
    Session::ptr s;
    if (!sid.empty()) {
        s = manager_->load(sid);
    }

    if (!s && options_.create_if_missing) {
        s = manager_->create();
    }

    if (s) {
        request->set_session(s);
    }

    return true;
}

void SessionMiddleware::after(const HttpRequest::ptr &request,
                              HttpResponse &response) {
    if (!manager_) {
        return;
    }

    auto s = request->session();
    if (!s) {
        return;
    }

    if (s->is_new() || s->modified()) {
        manager_->save(*s);

        HttpResponse::CookieOptions cookie_opt = options_.cookie;
        cookie_opt.max_age = static_cast<int>(manager_->options().ttl.count());

        response.set_cookie(options_.cookie_name, s->id(), cookie_opt);
        s->mark_persisted();
    }
}

} // namespace mid
} // namespace zhttp
