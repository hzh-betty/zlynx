#include "middleware.h"
#include "zhttp_logger.h"

#include <gtest/gtest.h>

using namespace zhttp;

// 测试中间件：记录调用顺序
class OrderTrackingMiddleware : public Middleware {
public:
  OrderTrackingMiddleware(std::vector<std::string> &log,
                          const std::string &name, bool pass = true)
      : log_(log), name_(name), pass_(pass) {}

  bool before(const HttpRequest::ptr &, HttpResponse &) override {
    log_.push_back(name_ + "_before");
    return pass_;
  }

  void after(const HttpRequest::ptr &, HttpResponse &) override {
    log_.push_back(name_ + "_after");
  }

private:
  std::vector<std::string> &log_;
  std::string name_;
  bool pass_;
};

TEST(MiddlewareTest, BeforeCalledInOrder) {
  std::vector<std::string> log;
  MiddlewareChain chain;

  chain.add(std::make_shared<OrderTrackingMiddleware>(log, "A"));
  chain.add(std::make_shared<OrderTrackingMiddleware>(log, "B"));
  chain.add(std::make_shared<OrderTrackingMiddleware>(log, "C"));

  auto request = std::make_shared<HttpRequest>();
  HttpResponse response;

  bool result = chain.execute_before(request, response);

  EXPECT_TRUE(result);
  ASSERT_EQ(log.size(), 3u);
  EXPECT_EQ(log[0], "A_before");
  EXPECT_EQ(log[1], "B_before");
  EXPECT_EQ(log[2], "C_before");
}

TEST(MiddlewareTest, AfterCalledInReverseOrder) {
  std::vector<std::string> log;
  MiddlewareChain chain;

  chain.add(std::make_shared<OrderTrackingMiddleware>(log, "A"));
  chain.add(std::make_shared<OrderTrackingMiddleware>(log, "B"));
  chain.add(std::make_shared<OrderTrackingMiddleware>(log, "C"));

  auto request = std::make_shared<HttpRequest>();
  HttpResponse response;

  chain.execute_before(request, response);
  log.clear();
  chain.execute_after(request, response);

  ASSERT_EQ(log.size(), 3u);
  EXPECT_EQ(log[0], "C_after");
  EXPECT_EQ(log[1], "B_after");
  EXPECT_EQ(log[2], "A_after");
}

TEST(MiddlewareTest, BeforeInterruptsChain) {
  std::vector<std::string> log;
  MiddlewareChain chain;

  chain.add(std::make_shared<OrderTrackingMiddleware>(log, "A"));
  chain.add(std::make_shared<OrderTrackingMiddleware>(log, "B", false)); // 中断
  chain.add(std::make_shared<OrderTrackingMiddleware>(log, "C"));

  auto request = std::make_shared<HttpRequest>();
  HttpResponse response;

  bool result = chain.execute_before(request, response);

  EXPECT_FALSE(result);
  ASSERT_EQ(log.size(), 2u);
  EXPECT_EQ(log[0], "A_before");
  EXPECT_EQ(log[1], "B_before");
  // C_before 不应被调用
}

TEST(MiddlewareTest, AfterOnlyCalledForExecutedMiddlewares) {
  std::vector<std::string> log;
  MiddlewareChain chain;

  chain.add(std::make_shared<OrderTrackingMiddleware>(log, "A"));
  chain.add(std::make_shared<OrderTrackingMiddleware>(log, "B", false)); // 中断
  chain.add(std::make_shared<OrderTrackingMiddleware>(log, "C"));

  auto request = std::make_shared<HttpRequest>();
  HttpResponse response;

  chain.execute_before(request, response);
  log.clear();
  chain.execute_after(request, response);

  // 只有 A 和 B 的 after 应该被调用（逆序）
  ASSERT_EQ(log.size(), 2u);
  EXPECT_EQ(log[0], "B_after");
  EXPECT_EQ(log[1], "A_after");
}

TEST(MiddlewareTest, EmptyChain) {
  MiddlewareChain chain;

  EXPECT_TRUE(chain.empty());
  EXPECT_EQ(chain.size(), 0u);

  auto request = std::make_shared<HttpRequest>();
  HttpResponse response;

  EXPECT_TRUE(chain.execute_before(request, response));
  chain.execute_after(request, response); // 不应崩溃
}

// 测试中间件：修改响应
class ResponseModifyMiddleware : public Middleware {
public:
  bool before(const HttpRequest::ptr &, HttpResponse &resp) override {
    resp.header("X-Before", "true");
    return true;
  }

  void after(const HttpRequest::ptr &, HttpResponse &resp) override {
    resp.header("X-After", "true");
  }
};

TEST(MiddlewareTest, MiddlewareCanModifyResponse) {
  MiddlewareChain chain;
  chain.add(std::make_shared<ResponseModifyMiddleware>());

  auto request = std::make_shared<HttpRequest>();
  HttpResponse response;

  chain.execute_before(request, response);
  EXPECT_EQ(response.headers().at("X-Before"), "true");

  chain.execute_after(request, response);
  EXPECT_EQ(response.headers().at("X-After"), "true");
}

int main(int argc, char **argv) {
  zhttp::init_logger();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
