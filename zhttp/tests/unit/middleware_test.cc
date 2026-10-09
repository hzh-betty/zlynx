#include "../support/request_builder.h"
#include <stdexcept>
#include <functional>
#include <vector>
using namespace zhttp;
namespace {
class FunctionMiddleware : public Middleware {
  public:
    using Before = std::function<bool(HttpContext &)>;
    using After = std::function<void(HttpContext &)>;
    explicit FunctionMiddleware(Before before = {}, After after = {})
        : before_(std::move(before)), after_(std::move(after)) {}
    bool before(HttpContext &context) override {
        return before_ ? before_(context) : true;
    }
    void after(HttpContext &context) override {
        if (after_)
            after_(context);
    }

  private:
    Before before_;
    After after_;
};
TEST(MiddlewareTest, BeforeAfterOrderAndShortCircuitAreRequestLocal) {
    HttpApplication pipeline;
    auto &router = pipeline.router();
    std::vector<int> trace;
    int inner_before = 0;
    pipeline.use(std::make_shared<FunctionMiddleware>(
        [&](HttpContext &) {
            trace.push_back(1);
            return true;
        },
        [&](HttpContext &) { trace.push_back(5); }));
    pipeline.use(std::make_shared<FunctionMiddleware>(
        [&](HttpContext &context) {
            trace.push_back(2);
            return context.header("Stop").empty();
        },
        [&](HttpContext &) { trace.push_back(4); }));
    pipeline.use(std::make_shared<FunctionMiddleware>(
        [&](HttpContext &) {
            ++inner_before;
            return true;
        }));
    router.get("/", [&](HttpContext &) { trace.push_back(3); });
    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    HttpContext context(request);
    EXPECT_TRUE(pipeline.handle(context));
    EXPECT_EQ(trace, (std::vector<int>{1, 2, 3, 4, 5}));
    trace.clear();
    auto stopped = std::make_shared<HttpRequest>();
    stopped->set_method(HttpMethod::GET);
    stopped->set_header("Stop", "yes");
    HttpContext second(stopped);
    pipeline.handle(second);
    EXPECT_EQ(trace, (std::vector<int>{1, 2, 4, 5}));
    EXPECT_EQ(inner_before, 1);
}
TEST(MiddlewareTest, ExceptionsResetStreamAndUpgradeBeforeAfterHooks) {
    HttpApplication pipeline;
    auto &router = pipeline.router();
    bool outer = false;
    pipeline.use(std::make_shared<FunctionMiddleware>(
        FunctionMiddleware::Before{}, [&](HttpContext &context) {
            outer = true;
            EXPECT_FALSE(context.upgrade());
            EXPECT_EQ(context.response().body_source().kind(),
                      HttpBody::Kind::Memory);
        }));
    router.get("/", [](HttpContext &context) {
        context.upgrade_to_websocket({});
        context.response().stream([](char *, size_t) { return 0U; });
        throw std::runtime_error("failure");
    });
    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    HttpContext context(request);
    pipeline.handle(context);
    EXPECT_TRUE(outer);
    EXPECT_EQ(context.response().status_code(),
              HttpStatus::INTERNAL_SERVER_ERROR);
}
TEST(MiddlewareTest, AfterHookCanReplaceUpgradeWithOrdinaryResponse) {
    for (auto status : {HttpStatus::OK, HttpStatus::UNAUTHORIZED,
                        HttpStatus::FORBIDDEN}) {
        HttpApplication pipeline;
        auto &router = pipeline.router();
        pipeline.use(std::make_shared<FunctionMiddleware>(
            FunctionMiddleware::Before{}, [status](HttpContext &context) {
                context.response().status(status).text("ordinary response");
                context.response().header("X-Middleware", "retained");
            }));
        router.get("/", [](HttpContext &context) {
            context.upgrade_to_websocket({});
        });
        auto request = std::make_shared<HttpRequest>();
        request->set_method(HttpMethod::GET);
        HttpContext context(request);
        ASSERT_TRUE(pipeline.handle(context));
        EXPECT_FALSE(context.upgrade());
        EXPECT_EQ(context.response().status_code(), status);
        EXPECT_EQ(context.response().body_content(), "ordinary response");
        EXPECT_EQ(context.response().headers().get("X-Middleware"),
                  "retained");
    }
}
TEST(MiddlewareTest, UpgradeWithNonemptyBodyIsRejectedBeforeCommit) {
    HttpApplication pipeline;
    auto &router = pipeline.router();
    router.get("/", [](HttpContext &context) {
        context.upgrade_to_websocket({});
        context.response().text("invalid upgrade body");
    });
    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    HttpContext context(request);
    ASSERT_TRUE(pipeline.handle(context));
    EXPECT_FALSE(context.upgrade());
    EXPECT_EQ(context.response().status_code(),
              HttpStatus::INTERNAL_SERVER_ERROR);
    EXPECT_FALSE(context.response().committed());
}
TEST(MiddlewareTest, BeforeExceptionStopsLaterHooksAndRunsOuterAfter) {
    HttpApplication pipeline;
    auto &router = pipeline.router();
    std::vector<int> trace;
    pipeline.use(std::make_shared<FunctionMiddleware>(
        [&](HttpContext &) {
            trace.push_back(1);
            return true;
        },
        [&](HttpContext &context) {
            trace.push_back(4);
            EXPECT_EQ(context.response().status_code(),
                      HttpStatus::INTERNAL_SERVER_ERROR);
        }));
    pipeline.use(std::make_shared<FunctionMiddleware>(
        [&](HttpContext &) -> bool {
            trace.push_back(2);
            throw std::runtime_error("before failure");
        },
        [&](HttpContext &) { ADD_FAILURE() << "Throwing before ran after"; }));
    pipeline.use(std::make_shared<FunctionMiddleware>(
        [&](HttpContext &) {
            ADD_FAILURE() << "Later before ran";
            return true;
        }));
    router.get("/", [&](HttpContext &) { trace.push_back(3); });
    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    HttpContext context(request);
    EXPECT_TRUE(pipeline.handle(context));
    EXPECT_EQ(trace, (std::vector<int>{1, 2, 4}));
}
TEST(MiddlewareTest, AfterExceptionResetsResultAndContinuesOuterCleanup) {
    HttpApplication pipeline;
    auto &router = pipeline.router();
    std::vector<int> trace;
    pipeline.use(std::make_shared<FunctionMiddleware>(
        FunctionMiddleware::Before{}, [&](HttpContext &context) {
            trace.push_back(3);
            EXPECT_FALSE(context.upgrade());
            EXPECT_EQ(context.response().body_source().kind(),
                      HttpBody::Kind::Memory);
            EXPECT_EQ(context.response().status_code(),
                      HttpStatus::INTERNAL_SERVER_ERROR);
        }));
    pipeline.use(std::make_shared<FunctionMiddleware>(
        FunctionMiddleware::Before{}, [&](HttpContext &context) {
            trace.push_back(2);
            context.upgrade_to_websocket({});
            context.response().stream([](char *, size_t) { return 0U; });
            throw 42;
        }));
    router.get("/", [&](HttpContext &) { trace.push_back(1); });
    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    HttpContext context(request);
    EXPECT_TRUE(pipeline.handle(context));
    EXPECT_EQ(trace, (std::vector<int>{1, 2, 3}));
}
TEST(MiddlewareTest, NestedRequestKeepsUnwindCountsIndependent) {
    HttpApplication pipeline;
    auto &router = pipeline.router();
    std::vector<std::string> trace;
    pipeline.use(std::make_shared<FunctionMiddleware>(
        FunctionMiddleware::Before{}, [&](HttpContext &context) {
            trace.push_back(context.path() + ":first");
        }));
    pipeline.use(std::make_shared<FunctionMiddleware>(
        [](HttpContext &context) { return context.path() != "/inner"; },
        [&](HttpContext &context) {
            trace.push_back(context.path() + ":second");
        }));
    pipeline.use(std::make_shared<FunctionMiddleware>(
        FunctionMiddleware::Before{}, [&](HttpContext &context) {
            trace.push_back(context.path() + ":third");
        }));
    router.get("/inner", [](HttpContext &) {
        ADD_FAILURE() << "Short-circuited inner handler ran";
    });
    router.get("/outer", [&](HttpContext &) {
        auto request = std::make_shared<HttpRequest>();
        request->set_method(HttpMethod::GET);
        request->set_target("/inner");
        HttpContext inner(request);
        EXPECT_TRUE(pipeline.handle(inner));
    });
    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    request->set_target("/outer");
    HttpContext context(request);
    EXPECT_TRUE(pipeline.handle(context));
    EXPECT_EQ(trace, (std::vector<std::string>{
                         "/inner:second", "/inner:first", "/outer:third",
                         "/outer:second", "/outer:first"}));
}
TEST(MiddlewareTest, DefaultHooksAndEmptyPipelineCallHandlerOnce) {
    for (bool use_middleware : {false, true}) {
        HttpApplication pipeline;
        auto &router = pipeline.router();
        int calls = 0;
        if (use_middleware)
            pipeline.use(std::make_shared<Middleware>());
        router.get("/", [&](HttpContext &) { ++calls; });
        auto request = std::make_shared<HttpRequest>();
        request->set_method(HttpMethod::GET);
        HttpContext context(request);
        EXPECT_TRUE(pipeline.handle(context));
        EXPECT_EQ(calls, 1);
    }
}
} // namespace
int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);

    return RUN_ALL_TESTS();
}
