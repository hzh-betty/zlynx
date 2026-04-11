#include <gtest/gtest.h>

#include "zhttp/mid/rate_limiter_middleware.h"
#include "zhttp/router.h"

using namespace zhttp;
using namespace zhttp::mid;


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

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
