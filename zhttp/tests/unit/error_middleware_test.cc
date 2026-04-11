#include "zhttp/mid/error_middleware.h"
#include "zhttp/zhttp_logger.h"

#include <gtest/gtest.h>

using namespace zhttp;
using namespace zhttp::mid;

TEST(ErrorMiddlewareTest, FormatsNotFoundAsJsonWhenBodyEmpty) {
    ErrorMiddleware middleware;

    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    request->set_path("/missing");

    HttpResponse response;
    response.status(HttpStatus::NOT_FOUND);

    EXPECT_TRUE(middleware.before(request, response));
    middleware.after(request, response);

    EXPECT_EQ(response.status_code(), HttpStatus::NOT_FOUND);
    EXPECT_NE(response.body_content().find("\"code\":404"), std::string::npos);
    EXPECT_NE(response.body_content().find("\"message\":\"Not Found\""),
              std::string::npos);
    EXPECT_NE(response.body_content().find("\"method\":\"GET\""),
              std::string::npos);
    EXPECT_NE(response.body_content().find("\"path\":\"/missing\""),
              std::string::npos);
}

TEST(ErrorMiddlewareTest, KeepsExistingErrorBodyByDefault) {
    ErrorMiddleware middleware;

    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::POST);
    request->set_path("/submit");

    HttpResponse response;
    response.status(HttpStatus::BAD_REQUEST).text("custom bad request");

    middleware.after(request, response);

    EXPECT_EQ(response.status_code(), HttpStatus::BAD_REQUEST);
    EXPECT_EQ(response.body_content(), "custom bad request");
}

TEST(ErrorMiddlewareTest, MasksServerErrorMessageByDefault) {
    ErrorMiddleware middleware;

    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    request->set_path("/panic");

    HttpResponse response;
    response.status(HttpStatus::INTERNAL_SERVER_ERROR);

    middleware.after(request, response);

    EXPECT_EQ(response.status_code(), HttpStatus::INTERNAL_SERVER_ERROR);
    EXPECT_NE(response.body_content().find("\"code\":500"), std::string::npos);
    EXPECT_NE(response.body_content().find("Internal Server Error"),
              std::string::npos);
}

TEST(ErrorMiddlewareTest, CanForceOverrideExistingErrorBody) {
    ErrorMiddleware::Options options;
    options.only_format_when_body_empty = false;
    options.include_method = false;
    options.include_path = true;

    ErrorMiddleware middleware(options);

    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::DELETE);
    request->set_path("/resource/1");

    HttpResponse response;
    response.status(HttpStatus::FORBIDDEN).text("will be replaced");

    middleware.after(request, response);

    EXPECT_EQ(response.status_code(), HttpStatus::FORBIDDEN);
    EXPECT_NE(response.body_content().find("\"code\":403"), std::string::npos);
    EXPECT_EQ(response.body_content().find("\"method\":"), std::string::npos);
    EXPECT_NE(response.body_content().find("\"path\":\"/resource/1\""),
              std::string::npos);
}

int main(int argc, char **argv) {
    zhttp::init_logger();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}