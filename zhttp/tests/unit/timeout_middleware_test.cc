#include "../support/manual_clock.h"
#include "../support/request_builder.h"
#include "zhttp/middleware/timeout_middleware.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

using namespace zhttp;
using namespace zhttp::mid;

TEST(TimeoutMiddlewareTest, OverrideResponseWhenTimeoutExceeded) {
    zhttp::test::ManualClock clock;
    TimeoutMiddleware::Options options;
    options.timeout_ms = 10;
    TimeoutMiddleware middleware(options);

    auto request = std::make_shared<TestContext>();
    HttpResponse response;
    response.status(HttpStatus::OK).text("ok");

    request->response() = std::move(response);
    ASSERT_TRUE(middleware.before(*request));
    clock.advance(std::chrono::milliseconds(30));
    middleware.after(*request);
    response = std::move(request->response());

    EXPECT_EQ(response.status_code(), HttpStatus::GATEWAY_TIMEOUT);
    EXPECT_EQ(response.body_content(), "Gateway Timeout");
}

TEST(TimeoutMiddlewareTest, KeepResponseWhenWithinTimeout) {
    zhttp::test::ManualClock clock;
    TimeoutMiddleware::Options options;
    options.timeout_ms = 100;
    TimeoutMiddleware middleware(options);

    auto request = std::make_shared<TestContext>();
    HttpResponse response;
    response.status(HttpStatus::OK).text("ok");

    request->response() = std::move(response);
    ASSERT_TRUE(middleware.before(*request));
    clock.advance(std::chrono::milliseconds(5));
    middleware.after(*request);
    response = std::move(request->response());

    EXPECT_EQ(response.status_code(), HttpStatus::OK);
    EXPECT_EQ(response.body_content(), "ok");
}

TEST(TimeoutMiddlewareTest, KeepExistingErrorByDefault) {
    zhttp::test::ManualClock clock;
    TimeoutMiddleware::Options options;
    options.timeout_ms = 10;
    TimeoutMiddleware middleware(options);

    auto request = std::make_shared<TestContext>();
    HttpResponse response;
    response.status(HttpStatus::BAD_REQUEST).text("bad request");

    request->response() = std::move(response);
    ASSERT_TRUE(middleware.before(*request));
    clock.advance(std::chrono::milliseconds(30));
    middleware.after(*request);
    response = std::move(request->response());

    EXPECT_EQ(response.status_code(), HttpStatus::BAD_REQUEST);
    EXPECT_EQ(response.body_content(), "bad request");
}

TEST(TimeoutMiddlewareTest, CustomTimeoutHandlerWorks) {
    zhttp::test::ManualClock clock;
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
    clock.advance(std::chrono::milliseconds(20));
    middleware.after(*request);
    response = std::move(request->response());

    EXPECT_EQ(response.status_code(), HttpStatus::SERVICE_UNAVAILABLE);
    EXPECT_NE(response.body_content().find("timeout:"), std::string::npos);
}

TEST(TimeoutMiddlewareTest, AfterConsumesStartOnceAndAllowsContextReuse) {
    zhttp::test::ManualClock clock;
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
    clock.advance(std::chrono::milliseconds(5));
    middleware.after(context);
    middleware.after(context);
    EXPECT_EQ(calls, 1);
    ASSERT_TRUE(middleware.before(context));
    clock.advance(std::chrono::milliseconds(5));
    middleware.after(context);
    EXPECT_EQ(calls, 2);
}

TEST(TimeoutMiddlewareTest, SharedInstanceSeparatesOverlappingContexts) {
    zhttp::test::ManualClock clock;
    TimeoutMiddleware::Options options;
    options.timeout_ms = 100;
    TimeoutMiddleware middleware(options);
    auto request = std::make_shared<HttpRequest>();
    HttpContext first(request), second(request);
    ASSERT_TRUE(middleware.before(first));
    clock.advance(std::chrono::milliseconds(150));
    ASSERT_TRUE(middleware.before(second));
    middleware.after(first);
    middleware.after(second);
    EXPECT_EQ(first.response().status_code(), HttpStatus::GATEWAY_TIMEOUT);
    EXPECT_EQ(second.response().status_code(), HttpStatus::OK);
}

TEST(TimeoutMiddlewareTest, SameInstanceCanBeRegisteredGloballyAndForAPath) {
    zhttp::test::ManualClock clock;

    class DelayAfter : public Middleware {
      public:
        explicit DelayAfter(zhttp::test::ManualClock &clock) : clock_(clock) {}

        void after(HttpContext &) override {
            clock_.advance(std::chrono::milliseconds(30));
        }

      private:
        zhttp::test::ManualClock &clock_;
    };

    TimeoutMiddleware::Options options;
    options.timeout_ms = 10;
    auto middleware = std::make_shared<TimeoutMiddleware>(options);
    HttpApplication pipeline;
    auto &router = pipeline.router();
    pipeline.use(middleware);
    pipeline.use(std::make_shared<DelayAfter>(clock));
    pipeline.use("/", middleware);
    router.get("/",
               [](HttpContext &context) { context.response().text("ok"); });
    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    HttpContext context(request);
    EXPECT_TRUE(pipeline.handle(context));
    EXPECT_EQ(context.response().status_code(), HttpStatus::GATEWAY_TIMEOUT);
}

TEST(TimeoutMiddlewareTest, ExactBoundaryIsAllowedAndNextMillisecondTimesOut) {
    zhttp::test::ManualClock clock;
    TimeoutMiddleware::Options options;
    options.timeout_ms = 100;
    TimeoutMiddleware middleware(options);
    TestContext exact, exceeded;
    ASSERT_TRUE(middleware.before(exact));
    ASSERT_TRUE(middleware.before(exceeded));
    clock.advance(std::chrono::milliseconds(100));
    middleware.after(exact);
    EXPECT_EQ(exact.response().status_code(), HttpStatus::OK);
    clock.advance(std::chrono::milliseconds(1));
    middleware.after(exceeded);
    EXPECT_EQ(exceeded.response().status_code(), HttpStatus::GATEWAY_TIMEOUT);
}

TEST(TimeoutMiddlewareTest, ZeroDisablesTimeoutAndOverrideCanReplaceErrors) {
    zhttp::test::ManualClock clock;
    TimeoutMiddleware::Options options;
    options.timeout_ms = 0;
    TimeoutMiddleware disabled(options);
    TestContext context;
    context.response().status(HttpStatus::BAD_REQUEST).text("bad");
    ASSERT_TRUE(disabled.before(context));
    clock.advance(std::chrono::hours(1));
    disabled.after(context);
    EXPECT_EQ(context.response().body_content(), "bad");
    options.timeout_ms = 10;
    options.override_non_error_only = false;
    TimeoutMiddleware overriding(options);
    ASSERT_TRUE(overriding.before(context));
    clock.advance(std::chrono::milliseconds(11));
    overriding.after(context);
    EXPECT_EQ(context.response().status_code(), HttpStatus::GATEWAY_TIMEOUT);
}
