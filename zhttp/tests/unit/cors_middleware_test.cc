#include "zhttp/mid/cors_middleware.h"
#include "zhttp/zhttp_logger.h"

#include <gtest/gtest.h>

using namespace zhttp;
using namespace zhttp::mid;

TEST(CorsMiddlewareTest, PreflightShortCircuitAndReflectRequestHeaders) {
    CorsMiddleware::Options opt;
    opt.allow_origins = {"https://frontend.example.com"};
    opt.allow_methods = {"GET", "POST", "OPTIONS"};
    opt.allow_headers.clear(); // 为空时回显 Access-Control-Request-Headers
    opt.allow_credentials = true;
    opt.max_age = 1200;
    opt.short_circuit_preflight = true;

    CorsMiddleware middleware(opt);

    auto req = std::make_shared<HttpRequest>();
    req->set_method(HttpMethod::OPTIONS);
    req->set_header("Origin", "https://frontend.example.com");
    req->set_header("Access-Control-Request-Method", "POST");
    req->set_header("Access-Control-Request-Headers", "Authorization, X-Trace");

    HttpResponse resp;
    const bool should_continue = middleware.before(req, resp);

    EXPECT_FALSE(should_continue);
    EXPECT_EQ(resp.status_code(), HttpStatus::NO_CONTENT);
    EXPECT_EQ(resp.headers().at("Access-Control-Allow-Origin"),
              "https://frontend.example.com");
    EXPECT_EQ(resp.headers().at("Access-Control-Allow-Credentials"), "true");
    EXPECT_EQ(resp.headers().at("Access-Control-Allow-Headers"),
              "Authorization, X-Trace");
    EXPECT_EQ(resp.headers().at("Access-Control-Allow-Methods"),
              "GET, POST, OPTIONS");
    EXPECT_EQ(resp.headers().at("Access-Control-Max-Age"), "1200");
}

TEST(CorsMiddlewareTest, NormalRequestAddCorsHeadersInAfter) {
    CorsMiddleware::Options opt;
    opt.allow_origins = {"https://app.example.com"};
    opt.expose_headers = {"X-Request-Id", "X-Trace"};

    CorsMiddleware middleware(opt);

    auto req = std::make_shared<HttpRequest>();
    req->set_method(HttpMethod::GET);
    req->set_header("Origin", "https://app.example.com");

    HttpResponse resp;
    resp.status(HttpStatus::OK).json("{\"ok\":true}");

    EXPECT_TRUE(middleware.before(req, resp));
    middleware.after(req, resp);

    EXPECT_EQ(resp.headers().at("Access-Control-Allow-Origin"),
              "https://app.example.com");
    EXPECT_EQ(resp.headers().at("Access-Control-Expose-Headers"),
              "X-Request-Id, X-Trace");
    EXPECT_EQ(resp.headers().at("Vary"), "Origin");
}

TEST(CorsMiddlewareTest, RejectDisallowedOriginOnPreflight) {
    CorsMiddleware::Options opt;
    opt.allow_origins = {"https://allowed.example.com"};
    opt.forbid_disallowed_origin_on_preflight = true;

    CorsMiddleware middleware(opt);

    auto req = std::make_shared<HttpRequest>();
    req->set_method(HttpMethod::OPTIONS);
    req->set_header("Origin", "https://evil.example.com");
    req->set_header("Access-Control-Request-Method", "GET");

    HttpResponse resp;
    const bool should_continue = middleware.before(req, resp);

    EXPECT_FALSE(should_continue);
    EXPECT_EQ(resp.status_code(), HttpStatus::FORBIDDEN);
}

TEST(CorsMiddlewareTest, NonPreflightRequestPassesAndCanSkipCorsHeaders) {
    CorsMiddleware::Options opt;
    opt.allow_origins = {"https://allowed.example.com"};

    CorsMiddleware middleware(opt);

    auto req = std::make_shared<HttpRequest>();
    req->set_method(HttpMethod::GET);
    req->set_header("Origin", "https://evil.example.com");

    HttpResponse resp;
    EXPECT_TRUE(middleware.before(req, resp));
    middleware.after(req, resp);
    EXPECT_EQ(resp.headers().count("Access-Control-Allow-Origin"), 0U);
}

TEST(CorsMiddlewareTest, PreflightCanContinueWithConfiguredHeadersAndNoMaxAge) {
    CorsMiddleware::Options opt;
    opt.allow_origins = {"*"};
    opt.allow_methods.clear();
    opt.allow_headers = {"Authorization", "X-Trace"};
    opt.max_age = 0;
    opt.short_circuit_preflight = false;

    CorsMiddleware middleware(opt);
    auto req = std::make_shared<HttpRequest>();
    req->set_method(HttpMethod::OPTIONS);
    req->set_header("Origin", "https://frontend.example.com");
    req->set_header("Access-Control-Request-Method", "POST");

    HttpResponse resp;
    const bool should_continue = middleware.before(req, resp);
    EXPECT_TRUE(should_continue);
    EXPECT_EQ(resp.headers().at("Access-Control-Allow-Origin"),
              "https://frontend.example.com");
    EXPECT_EQ(resp.headers().at("Access-Control-Allow-Headers"),
              "Authorization, X-Trace");
    EXPECT_EQ(resp.headers().count("Access-Control-Allow-Methods"), 0U);
    EXPECT_EQ(resp.headers().count("Allow"), 0U);
    EXPECT_EQ(resp.headers().count("Access-Control-Max-Age"), 0U);
}

TEST(CorsMiddlewareTest,
     DisallowedPreflightCanShortCircuitWithoutForbiddenWhenConfigured) {
    CorsMiddleware::Options opt;
    opt.allow_origins = {"https://allowed.example.com"};
    opt.forbid_disallowed_origin_on_preflight = false;
    opt.short_circuit_preflight = true;

    CorsMiddleware middleware(opt);

    auto req = std::make_shared<HttpRequest>();
    req->set_method(HttpMethod::OPTIONS);
    req->set_header("Origin", "https://evil.example.com");
    req->set_header("Access-Control-Request-Method", "GET");

    HttpResponse resp;
    const bool should_continue = middleware.before(req, resp);
    EXPECT_FALSE(should_continue);
    EXPECT_EQ(resp.status_code(), HttpStatus::NO_CONTENT);
    EXPECT_EQ(resp.headers().count("Access-Control-Allow-Origin"), 0U);
}

TEST(CorsMiddlewareTest, EmptyOriginFallsBackToWildcardAndNoVary) {
    CorsMiddleware::Options opt;
    opt.allow_origins = {"*"};
    opt.add_vary_origin = true;

    CorsMiddleware middleware(opt);
    auto req = std::make_shared<HttpRequest>();
    req->set_method(HttpMethod::GET);

    HttpResponse resp;
    middleware.after(req, resp);
    EXPECT_EQ(resp.headers().at("Access-Control-Allow-Origin"), "*");
    EXPECT_EQ(resp.headers().count("Vary"), 0U);
}

TEST(CorsMiddlewareTest, VaryOriginIsNotDuplicatedAndCanBeDisabled) {
    CorsMiddleware::Options opt;
    opt.allow_origins = {"https://app.example.com"};
    opt.add_vary_origin = true;
    CorsMiddleware middleware(opt);

    auto req = std::make_shared<HttpRequest>();
    req->set_method(HttpMethod::GET);
    req->set_header("Origin", "https://app.example.com");

    HttpResponse resp;
    resp.header("Vary", "Accept-Encoding, origin");
    middleware.after(req, resp);
    EXPECT_EQ(resp.headers().at("Vary"), "Accept-Encoding, origin");

    CorsMiddleware::Options no_vary_opt = opt;
    no_vary_opt.add_vary_origin = false;
    CorsMiddleware no_vary_middleware(no_vary_opt);

    HttpResponse no_vary_resp;
    no_vary_middleware.after(req, no_vary_resp);
    EXPECT_EQ(no_vary_resp.headers().count("Vary"), 0U);
}

TEST(CorsMiddlewareTest, OptionsWithoutOriginOrRequestMethodAreNotPreflight) {
    CorsMiddleware middleware(CorsMiddleware::Options{});

    {
        auto req = std::make_shared<HttpRequest>();
        req->set_method(HttpMethod::OPTIONS);
        HttpResponse resp;
        EXPECT_TRUE(middleware.before(req, resp));
    }

    {
        auto req = std::make_shared<HttpRequest>();
        req->set_method(HttpMethod::OPTIONS);
        req->set_header("Origin", "https://site.example.com");
        HttpResponse resp;
        EXPECT_TRUE(middleware.before(req, resp));
    }
}

TEST(CorsMiddlewareTest,
     DisallowedPreflightCanContinueWhenNotForbiddenAndNotShortCircuited) {
    CorsMiddleware::Options opt;
    opt.allow_origins = {"https://allowed.example.com"};
    opt.forbid_disallowed_origin_on_preflight = false;
    opt.short_circuit_preflight = false;
    opt.allow_headers.clear();
    opt.max_age = 0;

    CorsMiddleware middleware(opt);

    auto req = std::make_shared<HttpRequest>();
    req->set_method(HttpMethod::OPTIONS);
    req->set_header("Origin", "https://blocked.example.com");
    req->set_header("Access-Control-Request-Method", "POST");
    HttpResponse resp;

    EXPECT_TRUE(middleware.before(req, resp));
    EXPECT_EQ(resp.headers().count("Access-Control-Allow-Origin"), 0U);
    EXPECT_EQ(resp.headers().count("Access-Control-Allow-Headers"), 0U);
}

TEST(CorsMiddlewareTest, PreflightWithoutReqHeadersKeepsAllowHeadersUnset) {
    CorsMiddleware::Options opt;
    opt.allow_origins = {"*"};
    opt.short_circuit_preflight = true;
    opt.allow_headers.clear();
    opt.allow_methods = {"GET", "POST", "OPTIONS"};

    CorsMiddleware middleware(opt);

    auto req = std::make_shared<HttpRequest>();
    req->set_method(HttpMethod::OPTIONS);
    req->set_header("Origin", "https://frontend.example.com");
    req->set_header("Access-Control-Request-Method", "GET");

    HttpResponse resp;
    const bool should_continue = middleware.before(req, resp);
    EXPECT_FALSE(should_continue);
    EXPECT_EQ(resp.status_code(), HttpStatus::NO_CONTENT);
    EXPECT_EQ(resp.headers().count("Access-Control-Allow-Headers"), 0U);
}

int main(int argc, char **argv) {
    zhttp::init_logger();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
