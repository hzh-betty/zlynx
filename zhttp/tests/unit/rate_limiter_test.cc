#include <gtest/gtest.h>

#include "zhttp/mid/rate_limiter_middleware.h"
#include "zhttp/router.h"

using namespace zhttp;
using namespace zhttp::mid;

namespace {

class StubRateLimiter final : public RateLimiter {
  public:
    bool allow = true;
    Milliseconds retry_after = TimerHelper::milliseconds(0);
    mutable std::string last_key;

    bool isAllowed(const std::string &key) override {
        last_key = key;
        return allow;
    }

    Milliseconds retryAfter(const std::string &key) const override {
        last_key = key;
        return retry_after;
    }
};

} // namespace

class RateLimiterTest : public ::testing::Test {
  protected:
    using Clock = RateLimiter::Clock;
    Clock::time_point t0_ = Clock::now();

    RateLimiter::NowFunc now() {
        return [this] { return t0_; };
    }

    void advance_ms(int64_t ms) { t0_ += std::chrono::milliseconds(ms); }
};

TEST_F(RateLimiterTest, FactoryCreatesInstances) {
    auto fw = RateLimiter::newRateLimiter(RateLimiter::Type::FIXED_WINDOW, 2,
                                          RateLimiter::TimeUnit::SECOND, now());
    auto sw = RateLimiter::newRateLimiter(RateLimiter::Type::SLIDING_WINDOW, 2,
                                          RateLimiter::TimeUnit::SECOND, now());
    auto tb = RateLimiter::newRateLimiter(RateLimiter::Type::TOKEN_BUCKET, 2,
                                          RateLimiter::TimeUnit::SECOND, now());
    ASSERT_NE(fw, nullptr);
    ASSERT_NE(sw, nullptr);
    ASSERT_NE(tb, nullptr);
}

TEST_F(RateLimiterTest, FactoryFallsBackForUnknownTypeAndUnit) {
    auto fallback =
        RateLimiter::newRateLimiter(static_cast<RateLimiter::Type>(999), 0,
                                    static_cast<RateLimiter::TimeUnit>(999));
    ASSERT_NE(fallback, nullptr);

    EXPECT_TRUE(fallback->isAllowed("fallback"));
    EXPECT_FALSE(fallback->isAllowed("fallback"));
    EXPECT_GT(fallback->retryAfter("fallback").count(), 0);
}

TEST_F(RateLimiterTest, FixedWindow_BlocksAfterCapacity_ThenResets) {
    auto limiter =
        RateLimiter::newRateLimiter(RateLimiter::Type::FIXED_WINDOW, 2,
                                    RateLimiter::TimeUnit::SECOND, now());

    EXPECT_TRUE(limiter->isAllowed("k"));
    EXPECT_TRUE(limiter->isAllowed("k"));
    EXPECT_FALSE(limiter->isAllowed("k"));

    // 窗口结束后应恢复
    advance_ms(1000);
    EXPECT_TRUE(limiter->isAllowed("k"));
}

TEST_F(RateLimiterTest, RetryAfterAndZeroCapacityPathsAreCovered) {
    auto fixed =
        RateLimiter::newRateLimiter(RateLimiter::Type::FIXED_WINDOW, 0,
                                    RateLimiter::TimeUnit::MILLISECOND, now());
    ASSERT_NE(fixed, nullptr);
    EXPECT_EQ(fixed->retryAfter("none").count(), 0);
    EXPECT_TRUE(fixed->isAllowed("fw"));
    EXPECT_FALSE(fixed->isAllowed("fw"));
    EXPECT_GT(fixed->retryAfter("fw").count(), 0);
    advance_ms(1);
    EXPECT_EQ(fixed->retryAfter("fw").count(), 0);

    auto sliding =
        RateLimiter::newRateLimiter(RateLimiter::Type::SLIDING_WINDOW, 0,
                                    RateLimiter::TimeUnit::SECOND, now());
    ASSERT_NE(sliding, nullptr);
    EXPECT_EQ(sliding->retryAfter("none").count(), 0);
    EXPECT_TRUE(sliding->isAllowed("sw"));
    EXPECT_FALSE(sliding->isAllowed("sw"));
    EXPECT_GT(sliding->retryAfter("sw").count(), 0);
    advance_ms(1000);
    EXPECT_EQ(sliding->retryAfter("sw").count(), 0);

    auto token_bucket = RateLimiter::newRateLimiter(
        RateLimiter::Type::TOKEN_BUCKET, 0, RateLimiter::TimeUnit::HOUR, now());
    ASSERT_NE(token_bucket, nullptr);
    EXPECT_EQ(token_bucket->retryAfter("none").count(), 0);
    EXPECT_TRUE(token_bucket->isAllowed("tb"));
    EXPECT_FALSE(token_bucket->isAllowed("tb"));
    EXPECT_GT(token_bucket->retryAfter("tb").count(), 0);
    advance_ms(3600 * 1000);
    EXPECT_TRUE(token_bucket->isAllowed("tb"));
}

TEST_F(RateLimiterTest, SlidingWindow_SmoothsBoundary) {
    auto limiter =
        RateLimiter::newRateLimiter(RateLimiter::Type::SLIDING_WINDOW, 2,
                                    RateLimiter::TimeUnit::SECOND, now());

    EXPECT_TRUE(limiter->isAllowed("k"));
    advance_ms(100);
    EXPECT_TRUE(limiter->isAllowed("k"));
    advance_ms(800);
    EXPECT_FALSE(limiter->isAllowed("k"));

    // 再过一点点，让最早的那次过期，应该恢复 1 个名额
    advance_ms(101);
    EXPECT_TRUE(limiter->isAllowed("k"));
}

TEST_F(RateLimiterTest, TokenBucket_AllowsBurstAndRefill) {
    // capacity=2 per second，桶容量=2
    auto limiter =
        RateLimiter::newRateLimiter(RateLimiter::Type::TOKEN_BUCKET, 2,
                                    RateLimiter::TimeUnit::SECOND, now());

    EXPECT_TRUE(limiter->isAllowed("k"));
    EXPECT_TRUE(limiter->isAllowed("k"));
    EXPECT_FALSE(limiter->isAllowed("k"));

    // 0.5s 后应补充 1 个 token
    advance_ms(500);
    EXPECT_TRUE(limiter->isAllowed("k"));
}

TEST_F(RateLimiterTest, Middleware_UsesKeyFuncIsolation) {
    auto limiter =
        RateLimiter::newRateLimiter(RateLimiter::Type::FIXED_WINDOW, 1,
                                    RateLimiter::TimeUnit::SECOND, now());

    RateLimiterMiddleware::Options opt;
    opt.limiter = limiter;
    opt.key_func = [](const HttpRequest::ptr &req) { return req->path(); };

    Router router;
    router.use(std::make_shared<RateLimiterMiddleware>(opt));
    router.get("/a", [](const HttpRequest::ptr &, HttpResponse &resp) {
        resp.text("a");
    });
    router.get("/b", [](const HttpRequest::ptr &, HttpResponse &resp) {
        resp.text("b");
    });

    auto ra1 = std::make_shared<HttpRequest>();
    ra1->set_method(HttpMethod::GET);
    ra1->set_path("/a");
    HttpResponse rpa1;
    router.route(ra1, rpa1);
    EXPECT_EQ(rpa1.status_code(), HttpStatus::OK);

    auto ra2 = std::make_shared<HttpRequest>();
    ra2->set_method(HttpMethod::GET);
    ra2->set_path("/a");
    HttpResponse rpa2;
    router.route(ra2, rpa2);
    EXPECT_EQ(rpa2.status_code(), HttpStatus::TOO_MANY_REQUESTS);

    // /b 使用不同 key，不应被 /a 的限流影响
    auto rb1 = std::make_shared<HttpRequest>();
    rb1->set_method(HttpMethod::GET);
    rb1->set_path("/b");
    HttpResponse rpb1;
    router.route(rb1, rpb1);
    EXPECT_EQ(rpb1.status_code(), HttpStatus::OK);
}

TEST_F(RateLimiterTest, MiddlewareDefaultKeyUsesRemoteXffAndGlobal) {
    auto limiter = std::make_shared<StubRateLimiter>();
    RateLimiterMiddleware::Options opt;
    opt.limiter = limiter;
    RateLimiterMiddleware middleware(opt);

    auto req = std::make_shared<HttpRequest>();
    HttpResponse resp;

    req->set_remote_addr("10.0.0.8");
    EXPECT_TRUE(middleware.before(req, resp));
    EXPECT_EQ(limiter->last_key, "10.0.0.8");

    req->set_remote_addr("");
    req->set_header("X-Forwarded-For", " 192.168.1.2 , 10.0.0.1 ");
    EXPECT_TRUE(middleware.before(req, resp));
    EXPECT_EQ(limiter->last_key, "192.168.1.2");

    req->set_header("X-Forwarded-For", " \t ");
    EXPECT_TRUE(middleware.before(req, resp));
    EXPECT_EQ(limiter->last_key, "global");
}

TEST_F(RateLimiterTest, MiddlewareWritesRetryAfterWithFloorAndCeiling) {
    auto limiter = std::make_shared<StubRateLimiter>();
    limiter->allow = false;

    RateLimiterMiddleware::Options opt;
    opt.limiter = limiter;
    opt.retry_after_header = "X-Retry";
    RateLimiterMiddleware middleware(opt);

    auto req = std::make_shared<HttpRequest>();
    req->set_remote_addr("198.51.100.7");

    {
        HttpResponse resp;
        limiter->retry_after = TimerHelper::milliseconds(0);
        EXPECT_FALSE(middleware.before(req, resp));
        EXPECT_EQ(resp.status_code(), HttpStatus::TOO_MANY_REQUESTS);
        ASSERT_TRUE(resp.headers().count("X-Retry") > 0);
        EXPECT_EQ(resp.headers().at("X-Retry"), "1");
    }

    {
        HttpResponse resp;
        limiter->retry_after = TimerHelper::milliseconds(1500);
        EXPECT_FALSE(middleware.before(req, resp));
        EXPECT_EQ(resp.status_code(), HttpStatus::TOO_MANY_REQUESTS);
        ASSERT_TRUE(resp.headers().count("X-Retry") > 0);
        EXPECT_EQ(resp.headers().at("X-Retry"), "2");
    }
}

TEST_F(RateLimiterTest, MiddlewareCoercesNonPositiveRetryAfterToOneSecond) {
    auto limiter = std::make_shared<StubRateLimiter>();
    limiter->allow = false;
    limiter->retry_after = TimerHelper::milliseconds(-123);

    RateLimiterMiddleware::Options opt;
    opt.limiter = limiter;
    RateLimiterMiddleware middleware(opt);

    auto req = std::make_shared<HttpRequest>();
    req->set_remote_addr("203.0.113.42");

    HttpResponse resp;
    EXPECT_FALSE(middleware.before(req, resp));
    EXPECT_EQ(resp.status_code(), HttpStatus::TOO_MANY_REQUESTS);
    ASSERT_TRUE(resp.headers().count("Retry-After") > 0);
    EXPECT_EQ(resp.headers().at("Retry-After"), "1");
}

TEST_F(RateLimiterTest, MiddlewareWorksWithDefaultLimiterAndKeyResolver) {
    RateLimiterMiddleware middleware(RateLimiterMiddleware::Options{});
    auto req = std::make_shared<HttpRequest>();
    req->set_remote_addr("192.0.2.10");

    HttpResponse resp;
    EXPECT_TRUE(middleware.before(req, resp));
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
