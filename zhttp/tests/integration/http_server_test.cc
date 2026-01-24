#include "http_server.h"
#include "zhttp_logger.h"

#include "address.h"
#include "io_scheduler.h"

#include <chrono>
#include <gtest/gtest.h>
#include <thread>

using namespace zhttp;

// 注意：集成测试需要网络环境，可能需要特殊配置
// 这里提供基本的路由和中间件测试

TEST(HttpServerIntegrationTest, RouteRegistration) {
  bool handler_called = false;
  Router router;

  router.get("/test",
             [&handler_called](const HttpRequest::ptr &, HttpResponse &resp) {
               handler_called = true;
               resp.status(HttpStatus::OK).text("OK");
             });

  // 模拟路由匹配
  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_path("/test");
  HttpResponse response;

  bool found = router.route(request, response);

  EXPECT_TRUE(found);
  EXPECT_TRUE(handler_called);
  EXPECT_EQ(response.status_code(), HttpStatus::OK);
}

TEST(HttpServerIntegrationTest, MiddlewareIntegration) {
  // 创建一个简单的中间件
  class TestMiddleware : public Middleware {
  public:
    TestMiddleware(bool &before_called, bool &after_called)
        : before_called_(before_called), after_called_(after_called) {}

    bool before(const HttpRequest::ptr &, HttpResponse &resp) override {
      before_called_ = true;
      resp.header("X-Test-Middleware", "before");
      return true;
    }

    void after(const HttpRequest::ptr &, HttpResponse &resp) override {
      after_called_ = true;
      resp.header("X-Test-After", "after");
    }

  private:
    bool &before_called_;
    bool &after_called_;
  };

  bool before_called = false;
  bool after_called = false;
  Router router;

  router.use(std::make_shared<TestMiddleware>(before_called, after_called));
  router.get("/middleware-test",
             [](const HttpRequest::ptr &, HttpResponse &resp) {
               resp.status(HttpStatus::OK).text("OK");
             });

  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_path("/middleware-test");
  HttpResponse response;

  router.route(request, response);

  EXPECT_TRUE(before_called);
  EXPECT_TRUE(after_called);
  EXPECT_EQ(response.headers().at("X-Test-Middleware"), "before");
  EXPECT_EQ(response.headers().at("X-Test-After"), "after");
}

TEST(HttpServerIntegrationTest, NotFoundRoute) {
  Router router;
  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_path("/nonexistent");
  HttpResponse response;

  bool found = router.route(request, response);

  EXPECT_FALSE(found);
  EXPECT_EQ(response.status_code(), HttpStatus::NOT_FOUND);
}

int main(int argc, char **argv) {
  zhttp::init_logger();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
