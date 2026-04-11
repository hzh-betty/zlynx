#include "cors_middleware.h"
#include "zhttp_logger.h"

#include <gtest/gtest.h>

using namespace zhttp;

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

int main(int argc, char **argv) {
    zhttp::init_logger();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
