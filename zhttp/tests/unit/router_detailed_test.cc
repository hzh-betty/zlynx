#include "router.h"
#include "zhttp_logger.h"

#include <gtest/gtest.h>

using namespace zhttp;

class RouterDetailedTest : public ::testing::Test {
protected:
  Router router_;
};

// ========== 路由优先级测试 ==========

TEST_F(RouterDetailedTest, StaticVsParamPriority) {
  int static_called = 0;
  int param_called = 0;

  router_.get("/users/admin", [&](const HttpRequest::ptr &, HttpResponse &) {
    static_called++;
  });

  router_.get("/users/:id", [&](const HttpRequest::ptr &, HttpResponse &) {
    param_called++;
  });

  auto req1 = std::make_shared<HttpRequest>();
  req1->set_method(HttpMethod::GET);
  req1->set_path("/users/admin");
  HttpResponse resp1;
  router_.route(req1, resp1);

  EXPECT_EQ(static_called, 1);
  EXPECT_EQ(param_called, 0);

  auto req2 = std::make_shared<HttpRequest>();
  req2->set_method(HttpMethod::GET);
  req2->set_path("/users/123");
  HttpResponse resp2;
  router_.route(req2, resp2);

  EXPECT_EQ(static_called, 1);
  EXPECT_EQ(param_called, 1);
}

// ========== 复杂路径参数测试 ==========

TEST_F(RouterDetailedTest, NestedPathParams) {
  std::string captured_org, captured_repo, captured_issue;

  router_.get("/orgs/:org/repos/:repo/issues/:issue",
              [&](const HttpRequest::ptr &req, HttpResponse &) {
                captured_org = req->path_param("org");
                captured_repo = req->path_param("repo");
                captured_issue = req->path_param("issue");
              });

  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_path("/orgs/google/repos/zhttp/issues/42");
  HttpResponse response;

  bool found = router_.route(request, response);

  EXPECT_TRUE(found);
  EXPECT_EQ(captured_org, "google");
  EXPECT_EQ(captured_repo, "zhttp");
  EXPECT_EQ(captured_issue, "42");
}

TEST_F(RouterDetailedTest, ParamWithSpecialChars) {
  std::string captured_id;

  router_.get("/items/:id", [&](const HttpRequest::ptr &req, HttpResponse &) {
    captured_id = req->path_param("id");
  });

  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_path("/items/abc-123_xyz");
  HttpResponse response;

  router_.route(request, response);
  EXPECT_EQ(captured_id, "abc-123_xyz");
}

// ========== 正则路由详细测试 ==========

TEST_F(RouterDetailedTest, RegexWithMultipleGroups) {
  std::string captured_year, captured_month, captured_day;

  router_.add_regex_route(HttpMethod::GET,
                          "^/archive/(\\d{4})/(\\d{2})/(\\d{2})$",
                          {"year", "month", "day"},
                          [&](const HttpRequest::ptr &req, HttpResponse &) {
                            captured_year = req->path_param("year");
                            captured_month = req->path_param("month");
                            captured_day = req->path_param("day");
                          });

  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_path("/archive/2024/01/15");
  HttpResponse response;

  bool found = router_.route(request, response);

  EXPECT_TRUE(found);
  EXPECT_EQ(captured_year, "2024");
  EXPECT_EQ(captured_month, "01");
  EXPECT_EQ(captured_day, "15");
}

TEST_F(RouterDetailedTest, RegexNotMatching) {
  bool handler_called = false;

  router_.add_regex_route(
      HttpMethod::GET, "^/api/v\\d+/users$", {},
      [&](const HttpRequest::ptr &, HttpResponse &) { handler_called = true; });

  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_path("/api/vX/users"); // 不匹配 \d+
  HttpResponse response;

  bool found = router_.route(request, response);

  EXPECT_FALSE(found);
  EXPECT_FALSE(handler_called);
}

// ========== 中间件链测试 ==========

TEST_F(RouterDetailedTest, MultipleGlobalMiddlewares) {
  std::vector<std::string> execution_order;

  class OrderMiddleware : public Middleware {
  public:
    OrderMiddleware(std::vector<std::string> &order, const std::string &name)
        : order_(order), name_(name) {}

    bool before(const HttpRequest::ptr &, HttpResponse &) override {
      order_.push_back(name_ + "_before");
      return true;
    }

    void after(const HttpRequest::ptr &, HttpResponse &) override {
      order_.push_back(name_ + "_after");
    }

  private:
    std::vector<std::string> &order_;
    std::string name_;
  };

  router_.use(std::make_shared<OrderMiddleware>(execution_order, "MW1"));
  router_.use(std::make_shared<OrderMiddleware>(execution_order, "MW2"));
  router_.use(std::make_shared<OrderMiddleware>(execution_order, "MW3"));

  router_.get("/test", [&](const HttpRequest::ptr &, HttpResponse &) {
    execution_order.push_back("handler");
  });

  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_path("/test");
  HttpResponse response;

  router_.route(request, response);

  ASSERT_EQ(execution_order.size(), 7u);
  EXPECT_EQ(execution_order[0], "MW1_before");
  EXPECT_EQ(execution_order[1], "MW2_before");
  EXPECT_EQ(execution_order[2], "MW3_before");
  EXPECT_EQ(execution_order[3], "handler");
  EXPECT_EQ(execution_order[4], "MW3_after");
  EXPECT_EQ(execution_order[5], "MW2_after");
  EXPECT_EQ(execution_order[6], "MW1_after");
}

TEST_F(RouterDetailedTest, MiddlewareInterruption) {
  bool handler_called = false;
  bool after_called = false;

  class BlockingMiddleware : public Middleware {
  public:
    bool before(const HttpRequest::ptr &, HttpResponse &resp) override {
      resp.status(HttpStatus::UNAUTHORIZED).text("Blocked");
      return false; // 中断
    }

    void after(const HttpRequest::ptr &, HttpResponse &) override {
      // 即使中断也会调用
    }
  };

  class AfterMiddleware : public Middleware {
  public:
    AfterMiddleware(bool &called) : called_(called) {}

    bool before(const HttpRequest::ptr &, HttpResponse &) override {
      return true;
    }

    void after(const HttpRequest::ptr &, HttpResponse &) override {
      called_ = true;
    }

  private:
    bool &called_;
  };

  router_.use(std::make_shared<AfterMiddleware>(after_called));
  router_.use(std::make_shared<BlockingMiddleware>());

  router_.get("/test", [&](const HttpRequest::ptr &, HttpResponse &) {
    handler_called = true;
  });

  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_path("/test");
  HttpResponse response;

  router_.route(request, response);

  EXPECT_FALSE(handler_called);
  EXPECT_TRUE(after_called); // after 仍然被调用
  EXPECT_EQ(response.status_code(), HttpStatus::UNAUTHORIZED);
}

// ========== HTTP方法路由测试 ==========

TEST_F(RouterDetailedTest, SamePathDifferentMethods) {
  std::string last_method;

  router_.get("/resource", [&](const HttpRequest::ptr &, HttpResponse &) {
    last_method = "GET";
  });

  router_.post("/resource", [&](const HttpRequest::ptr &, HttpResponse &) {
    last_method = "POST";
  });

  router_.put("/resource", [&](const HttpRequest::ptr &, HttpResponse &) {
    last_method = "PUT";
  });

  router_.del("/resource", [&](const HttpRequest::ptr &, HttpResponse &) {
    last_method = "DELETE";
  });

  auto test_method = [&](HttpMethod method, const std::string &expected) {
    auto req = std::make_shared<HttpRequest>();
    req->set_method(method);
    req->set_path("/resource");
    HttpResponse resp;
    router_.route(req, resp);
    EXPECT_EQ(last_method, expected);
  };

  test_method(HttpMethod::GET, "GET");
  test_method(HttpMethod::POST, "POST");
  test_method(HttpMethod::PUT, "PUT");
  test_method(HttpMethod::DELETE, "DELETE");
}

// ========== 404处理测试 ==========

TEST_F(RouterDetailedTest, Custom404Handler) {
  bool custom_404_called = false;

  router_.set_not_found_handler(
      [&](const HttpRequest::ptr &, HttpResponse &resp) {
        custom_404_called = true;
        resp.status(HttpStatus::NOT_FOUND).json("{\"error\":\"Custom 404\"}");
      });

  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_path("/nonexistent");
  HttpResponse response;

  bool found = router_.route(request, response);

  EXPECT_FALSE(found);
  EXPECT_TRUE(custom_404_called);
  EXPECT_EQ(response.status_code(), HttpStatus::NOT_FOUND);
  EXPECT_EQ(response.body_content(), "{\"error\":\"Custom 404\"}");
}

// ========== 路由冲突测试 ==========

TEST_F(RouterDetailedTest, OverwriteExistingRoute) {
  int first_handler_calls = 0;
  int second_handler_calls = 0;

  router_.get("/test", [&](const HttpRequest::ptr &, HttpResponse &) {
    first_handler_calls++;
  });

  // 覆盖同一路由
  router_.get("/test", [&](const HttpRequest::ptr &, HttpResponse &) {
    second_handler_calls++;
  });

  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_path("/test");
  HttpResponse response;

  router_.route(request, response);

  EXPECT_EQ(first_handler_calls, 0);
  EXPECT_EQ(second_handler_calls, 1);
}

// ========== 性能测试 ==========

TEST_F(RouterDetailedTest, ManyRoutes) {
  // 添加大量路由
  for (int i = 0; i < 1000; ++i) {
    std::string path = "/route" + std::to_string(i);
    router_.get(path, [](const HttpRequest::ptr &, HttpResponse &resp) {
      resp.status(HttpStatus::OK);
    });
  }

  // 测试查找性能
  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_path("/route500");
  HttpResponse response;

  bool found = router_.route(request, response);

  EXPECT_TRUE(found);
  EXPECT_EQ(response.status_code(), HttpStatus::OK);
}

int main(int argc, char **argv) {
  zhttp::init_logger();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
