#include "../test_support.h"
#include "zhttp/middleware/timeout_middleware.h"
#include "zhttp/zhttp_logger.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace zhttp;
using namespace zhttp::mid;

TEST(TimeoutMiddlewareTest, OverrideResponseWhenTimeoutExceeded) {
    TimeoutMiddleware::Options options;
    options.timeout_ms = 10;
    TimeoutMiddleware middleware(options);

    auto request = std::make_shared<TestContext>();
    HttpResponse response;
    response.status(HttpStatus::OK).text("ok");

    request->response() = std::move(response);
    ASSERT_TRUE(middleware.before(*request));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    middleware.after(*request);
    response = std::move(request->response());

    EXPECT_EQ(response.status_code(), HttpStatus::GATEWAY_TIMEOUT);
    EXPECT_EQ(response.body_content(), "Gateway Timeout");
}

TEST(TimeoutMiddlewareTest, KeepResponseWhenWithinTimeout) {
    TimeoutMiddleware::Options options;
    options.timeout_ms = 100;
    TimeoutMiddleware middleware(options);

    auto request = std::make_shared<TestContext>();
    HttpResponse response;
    response.status(HttpStatus::OK).text("ok");

    request->response() = std::move(response);
    ASSERT_TRUE(middleware.before(*request));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    middleware.after(*request);
    response = std::move(request->response());

    EXPECT_EQ(response.status_code(), HttpStatus::OK);
    EXPECT_EQ(response.body_content(), "ok");
}

TEST(TimeoutMiddlewareTest, KeepExistingErrorByDefault) {
    TimeoutMiddleware::Options options;
    options.timeout_ms = 10;
    TimeoutMiddleware middleware(options);

    auto request = std::make_shared<TestContext>();
    HttpResponse response;
    response.status(HttpStatus::BAD_REQUEST).text("bad request");

    request->response() = std::move(response);
    ASSERT_TRUE(middleware.before(*request));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    middleware.after(*request);
    response = std::move(request->response());

    EXPECT_EQ(response.status_code(), HttpStatus::BAD_REQUEST);
    EXPECT_EQ(response.body_content(), "bad request");
}

TEST(TimeoutMiddlewareTest, CustomTimeoutHandlerWorks) {
    TimeoutMiddleware::Options options;
    options.timeout_ms = 10;
    options.timeout_handler = [](HttpContext &context,
                                 std::chrono::milliseconds elapsed) {
        auto &response = context.response();
        response.status(HttpStatus::SERVICE_UNAVAILABLE)
            .text("timeout:" + std::to_string(elapsed.count()));
    };

    TimeoutMiddleware middleware(options);

    auto request = std::make_shared<TestContext>();
    HttpResponse response;
    response.status(HttpStatus::OK).text("ok");

    request->response() = std::move(response);
    ASSERT_TRUE(middleware.before(*request));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    middleware.after(*request);
    response = std::move(request->response());

    EXPECT_EQ(response.status_code(), HttpStatus::SERVICE_UNAVAILABLE);
    EXPECT_NE(response.body_content().find("timeout:"), std::string::npos);
}

TEST(TimeoutMiddlewareTest, AfterConsumesStartOnceAndAllowsContextReuse) {
    TimeoutMiddleware::Options options;
    options.timeout_ms = 1;
    int calls = 0;
    options.timeout_handler = [&](HttpContext &, std::chrono::milliseconds) {
        ++calls;
    };
    TimeoutMiddleware middleware(options);
    TestContext context;
    middleware.after(context);
    EXPECT_EQ(calls, 0);
    ASSERT_TRUE(middleware.before(context));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    middleware.after(context);
    middleware.after(context);
    EXPECT_EQ(calls, 1);
    ASSERT_TRUE(middleware.before(context));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    middleware.after(context);
    EXPECT_EQ(calls, 2);
}

TEST(TimeoutMiddlewareTest, SharedInstanceSeparatesOverlappingContexts) {
    TimeoutMiddleware::Options options;
    options.timeout_ms = 100;
    TimeoutMiddleware middleware(options);
    auto request = std::make_shared<HttpRequest>();
    HttpContext first(request), second(request);
    ASSERT_TRUE(middleware.before(first));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    ASSERT_TRUE(middleware.before(second));
    middleware.after(first);
    middleware.after(second);
    EXPECT_EQ(first.response().status_code(), HttpStatus::GATEWAY_TIMEOUT);
    EXPECT_EQ(second.response().status_code(), HttpStatus::OK);
}

TEST(TimeoutMiddlewareTest, SameInstanceCanBeRegisteredGloballyAndForAPath) {
    class DelayAfter : public Middleware {
      public:
        void after(HttpContext &) override {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    };
    TimeoutMiddleware::Options options;
    options.timeout_ms = 10;
    auto middleware = std::make_shared<TimeoutMiddleware>(options);
    RequestPipeline pipeline;
    Router router;
    pipeline.use(middleware);
    pipeline.use(std::make_shared<DelayAfter>());
    pipeline.use("/", middleware);
    router.get("/", [](HttpContext &context) { context.response().text("ok"); });
    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    HttpContext context(request);
    EXPECT_TRUE(pipeline.execute(context, router));
    EXPECT_EQ(context.response().status_code(), HttpStatus::GATEWAY_TIMEOUT);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    zhttp::init_logger();
    return RUN_ALL_TESTS();
}
