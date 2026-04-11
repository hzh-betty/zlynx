#include "zhttp/mid/timeout_middleware.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace zhttp;
using namespace zhttp::mid;

TEST(TimeoutMiddlewareTest, OverrideResponseWhenTimeoutExceeded) {
    TimeoutMiddleware::Options options;
    options.timeout_ms = 10;
    TimeoutMiddleware middleware(options);

    auto request = std::make_shared<HttpRequest>();
    HttpResponse response;
    response.status(HttpStatus::OK).text("ok");

    EXPECT_TRUE(middleware.before(request, response));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    middleware.after(request, response);

    EXPECT_EQ(response.status_code(), HttpStatus::GATEWAY_TIMEOUT);
    EXPECT_EQ(response.body_content(), "Gateway Timeout");
}

TEST(TimeoutMiddlewareTest, KeepResponseWhenWithinTimeout) {
    TimeoutMiddleware::Options options;
    options.timeout_ms = 100;
    TimeoutMiddleware middleware(options);

    auto request = std::make_shared<HttpRequest>();
    HttpResponse response;
    response.status(HttpStatus::OK).text("ok");

    EXPECT_TRUE(middleware.before(request, response));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    middleware.after(request, response);

    EXPECT_EQ(response.status_code(), HttpStatus::OK);
    EXPECT_EQ(response.body_content(), "ok");
}

TEST(TimeoutMiddlewareTest, KeepExistingErrorByDefault) {
    TimeoutMiddleware::Options options;
    options.timeout_ms = 10;
    TimeoutMiddleware middleware(options);

    auto request = std::make_shared<HttpRequest>();
    HttpResponse response;
    response.status(HttpStatus::BAD_REQUEST).text("bad request");

    EXPECT_TRUE(middleware.before(request, response));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    middleware.after(request, response);

    EXPECT_EQ(response.status_code(), HttpStatus::BAD_REQUEST);
    EXPECT_EQ(response.body_content(), "bad request");
}

TEST(TimeoutMiddlewareTest, CustomTimeoutHandlerWorks) {
    TimeoutMiddleware::Options options;
    options.timeout_ms = 10;
    options.timeout_handler = [](const HttpRequest::ptr &,
                                 HttpResponse &response,
                                 std::chrono::milliseconds elapsed) {
        response.status(HttpStatus::SERVICE_UNAVAILABLE)
            .text("timeout:" + std::to_string(elapsed.count()));
    };

    TimeoutMiddleware middleware(options);

    auto request = std::make_shared<HttpRequest>();
    HttpResponse response;
    response.status(HttpStatus::OK).text("ok");

    EXPECT_TRUE(middleware.before(request, response));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    middleware.after(request, response);

    EXPECT_EQ(response.status_code(), HttpStatus::SERVICE_UNAVAILABLE);
    EXPECT_NE(response.body_content().find("timeout:"), std::string::npos);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}