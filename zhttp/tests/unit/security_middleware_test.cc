#include "zhttp/mid/security_middleware.h"

#include <gtest/gtest.h>

using namespace zhttp;
using namespace zhttp::mid;

TEST(SecurityMiddlewareTest, AddDefaultSecurityHeaders) {
    SecurityMiddleware middleware;

    auto request = std::make_shared<HttpRequest>();
    HttpResponse response;

    EXPECT_TRUE(middleware.before(request, response));
    middleware.after(request, response);

    const auto &headers = response.headers();

    ASSERT_NE(headers.find("X-Frame-Options"), headers.end());
    EXPECT_EQ(headers.at("X-Frame-Options"), "DENY");

    ASSERT_NE(headers.find("X-Content-Type-Options"), headers.end());
    EXPECT_EQ(headers.at("X-Content-Type-Options"), "nosniff");

    ASSERT_NE(headers.find("Referrer-Policy"), headers.end());
    EXPECT_EQ(headers.at("Referrer-Policy"), "strict-origin-when-cross-origin");

    ASSERT_NE(headers.find("Content-Security-Policy"), headers.end());
    EXPECT_NE(headers.at("Content-Security-Policy").find("default-src 'self'"),
              std::string::npos);

    ASSERT_NE(headers.find("Permissions-Policy"), headers.end());
}

TEST(SecurityMiddlewareTest, KeepExistingHeadersByDefault) {
    SecurityMiddleware middleware;

    auto request = std::make_shared<HttpRequest>();
    HttpResponse response;
    response.header("X-Frame-Options", "SAMEORIGIN");
    response.header("Referrer-Policy", "no-referrer");

    middleware.after(request, response);

    EXPECT_EQ(response.headers().at("X-Frame-Options"), "SAMEORIGIN");
    EXPECT_EQ(response.headers().at("Referrer-Policy"), "no-referrer");
}

TEST(SecurityMiddlewareTest, HstsCanBeEnabled) {
    SecurityMiddleware::Options options;
    options.set_hsts = true;
    options.hsts = "max-age=86400";

    SecurityMiddleware middleware(options);

    auto request = std::make_shared<HttpRequest>();
    HttpResponse response;

    middleware.after(request, response);

    ASSERT_NE(response.headers().find("Strict-Transport-Security"),
              response.headers().end());
    EXPECT_EQ(response.headers().at("Strict-Transport-Security"),
              "max-age=86400");
}

TEST(SecurityMiddlewareTest, CanDisableSingleHeader) {
    SecurityMiddleware::Options options;
    options.set_content_security_policy = false;

    SecurityMiddleware middleware(options);

    auto request = std::make_shared<HttpRequest>();
    HttpResponse response;

    middleware.after(request, response);

    EXPECT_EQ(response.headers().find("Content-Security-Policy"),
              response.headers().end());
    ASSERT_NE(response.headers().find("X-Frame-Options"),
              response.headers().end());
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}