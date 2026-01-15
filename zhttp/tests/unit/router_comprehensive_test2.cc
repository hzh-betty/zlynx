#include "router.h"
#include "zhttp_logger.h"
#include <gtest/gtest.h>

using namespace zhttp;

class RouterComprehensiveTest2 : public ::testing::Test {
protected:
  void SetUp() override { init_logger(zlog::LogLevel::value::WARNING); }
  Router router_;

  HttpRequest::ptr make_request(HttpMethod method, const std::string &path) {
    auto req = std::make_shared<HttpRequest>();
    req->set_method(method);
    req->set_path(path);
    return req;
  }
};

// ===================== 正则路由测试 (41-60) =====================

TEST_F(RouterComprehensiveTest2, Regex_SimpleDigits) {
  router_.add_regex_route(HttpMethod::GET, "/users/(\\d+)", {"id"},
                          [](const HttpRequest::ptr &req, HttpResponse &resp) {
                            resp.text(req->path_param("id"));
                          });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/users/12345"), resp);
  EXPECT_EQ(resp.body_content(), "12345");
}

TEST_F(RouterComprehensiveTest2, Regex_NotMatchLetters) {
  router_.add_regex_route(
      HttpMethod::GET, "/users/(\\d+)", {"id"},
      [](const HttpRequest::ptr &, HttpResponse &resp) { resp.text("ok"); });
  HttpResponse resp;
  EXPECT_FALSE(
      router_.route(make_request(HttpMethod::GET, "/users/abc"), resp));
}

TEST_F(RouterComprehensiveTest2, Regex_MultipleCaptures) {
  router_.add_regex_route(HttpMethod::GET, "/date/(\\d{4})-(\\d{2})-(\\d{2})",
                          {"year", "month", "day"},
                          [](const HttpRequest::ptr &req, HttpResponse &resp) {
                            resp.text(req->path_param("year") + "/" +
                                      req->path_param("month") + "/" +
                                      req->path_param("day"));
                          });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/date/2024-01-15"), resp);
  EXPECT_EQ(resp.body_content(), "2024/01/15");
}

TEST_F(RouterComprehensiveTest2, Regex_AlphaNumeric) {
  router_.add_regex_route(HttpMethod::GET, "/items/([a-zA-Z0-9]+)", {"code"},
                          [](const HttpRequest::ptr &req, HttpResponse &resp) {
                            resp.text(req->path_param("code"));
                          });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/items/ABC123"), resp);
  EXPECT_EQ(resp.body_content(), "ABC123");
}

TEST_F(RouterComprehensiveTest2, Regex_OptionalPart) {
  router_.add_regex_route(HttpMethod::GET, "/api/v(\\d+)(?:/.*)?", {"version"},
                          [](const HttpRequest::ptr &req, HttpResponse &resp) {
                            resp.text(req->path_param("version"));
                          });
  HttpResponse resp1, resp2;
  router_.route(make_request(HttpMethod::GET, "/api/v1"), resp1);
  router_.route(make_request(HttpMethod::GET, "/api/v2/extra"), resp2);
  EXPECT_EQ(resp1.body_content(), "1");
  EXPECT_EQ(resp2.body_content(), "2");
}

TEST_F(RouterComprehensiveTest2, Regex_EmailLike) {
  router_.add_regex_route(
      HttpMethod::GET, "/profile/([a-z]+)@([a-z]+)\\.com", {"user", "domain"},
      [](const HttpRequest::ptr &req, HttpResponse &resp) {
        resp.text(req->path_param("user") + "@" + req->path_param("domain"));
      });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/profile/john@example.com"),
                resp);
  EXPECT_EQ(resp.body_content(), "john@example");
}

TEST_F(RouterComprehensiveTest2, Regex_PrefixBucket_SamePrefix) {
  router_.add_regex_route(
      HttpMethod::GET, "/api/v1/users/(\\d+)", {"id"},
      [](const HttpRequest::ptr &, HttpResponse &resp) { resp.text("users"); });
  router_.add_regex_route(
      HttpMethod::GET, "/api/v1/users/(\\d+)/posts", {"id"},
      [](const HttpRequest::ptr &, HttpResponse &resp) { resp.text("posts"); });

  HttpResponse resp1, resp2;
  router_.route(make_request(HttpMethod::GET, "/api/v1/users/1"), resp1);
  router_.route(make_request(HttpMethod::GET, "/api/v1/users/1/posts"), resp2);
  EXPECT_EQ(resp1.body_content(), "users");
  EXPECT_EQ(resp2.body_content(), "posts");
}

TEST_F(RouterComprehensiveTest2, Regex_PrefixBucket_DifferentPrefix) {
  router_.add_regex_route(
      HttpMethod::GET, "/api/v1/items/(\\d+)", {"id"},
      [](const HttpRequest::ptr &, HttpResponse &resp) { resp.text("v1"); });
  router_.add_regex_route(
      HttpMethod::GET, "/api/v2/items/(\\d+)", {"id"},
      [](const HttpRequest::ptr &, HttpResponse &resp) { resp.text("v2"); });

  HttpResponse resp1, resp2;
  router_.route(make_request(HttpMethod::GET, "/api/v1/items/1"), resp1);
  router_.route(make_request(HttpMethod::GET, "/api/v2/items/2"), resp2);
  EXPECT_EQ(resp1.body_content(), "v1");
  EXPECT_EQ(resp2.body_content(), "v2");
}

TEST_F(RouterComprehensiveTest2, Regex_PostMethod) {
  router_.add_regex_route(HttpMethod::POST, "/submit/(\\d+)", {"id"},
                          [](const HttpRequest::ptr &req, HttpResponse &resp) {
                            resp.text("post-" + req->path_param("id"));
                          });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::POST, "/submit/999"), resp);
  EXPECT_EQ(resp.body_content(), "post-999");
}

TEST_F(RouterComprehensiveTest2, Regex_DynamicPriority) {
  router_.get("/items/:id", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("dynamic");
  });
  router_.add_regex_route(
      HttpMethod::GET, "/items/(\\d+)", {"id"},
      [](const HttpRequest::ptr &, HttpResponse &resp) { resp.text("regex"); });

  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/items/123"), resp);
  EXPECT_EQ(resp.body_content(), "dynamic"); // Dynamic has priority
}

TEST_F(RouterComprehensiveTest2, Regex_UUID) {
  router_.add_regex_route(
      HttpMethod::GET,
      "/resources/"
      "([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})",
      {"uuid"}, [](const HttpRequest::ptr &req, HttpResponse &resp) {
        resp.text(req->path_param("uuid"));
      });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET,
                             "/resources/550e8400-e29b-41d4-a716-446655440000"),
                resp);
  EXPECT_EQ(resp.body_content(), "550e8400-e29b-41d4-a716-446655440000");
}

TEST_F(RouterComprehensiveTest2, Regex_FileExtension) {
  router_.add_regex_route(
      HttpMethod::GET, "/files/(.+)\\.(jpg|png|gif)", {"name", "ext"},
      [](const HttpRequest::ptr &req, HttpResponse &resp) {
        resp.text(req->path_param("name") + "." + req->path_param("ext"));
      });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/files/photo.jpg"), resp);
  EXPECT_EQ(resp.body_content(), "photo.jpg");
}

TEST_F(RouterComprehensiveTest2, Regex_ManyRoutes) {
  for (int i = 0; i < 20; ++i) {
    router_.add_regex_route(
        HttpMethod::GET, "/prefix" + std::to_string(i) + "/(\\d+)", {"id"},
        [i](const HttpRequest::ptr &req, HttpResponse &resp) {
          resp.text(std::to_string(i) + "-" + req->path_param("id"));
        });
  }
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/prefix15/42"), resp);
  EXPECT_EQ(resp.body_content(), "15-42");
}

TEST_F(RouterComprehensiveTest2, Regex_NoCapture) {
  router_.add_regex_route(HttpMethod::GET, "/status/\\d+", {},
                          [](const HttpRequest::ptr &, HttpResponse &resp) {
                            resp.text("matched");
                          });
  HttpResponse resp;
  EXPECT_TRUE(
      router_.route(make_request(HttpMethod::GET, "/status/200"), resp));
}

TEST_F(RouterComprehensiveTest2, Regex_Anchored) {
  router_.add_regex_route(
      HttpMethod::GET, "/exact/(\\d+)", {"id"},
      [](const HttpRequest::ptr &, HttpResponse &resp) { resp.text("ok"); });
  HttpResponse resp;
  EXPECT_FALSE(
      router_.route(make_request(HttpMethod::GET, "/exact/123/extra"), resp));
}

TEST_F(RouterComprehensiveTest2, Regex_Word) {
  router_.add_regex_route(HttpMethod::GET, "/words/(\\w+)", {"word"},
                          [](const HttpRequest::ptr &req, HttpResponse &resp) {
                            resp.text(req->path_param("word"));
                          });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/words/hello_world123"), resp);
  EXPECT_EQ(resp.body_content(), "hello_world123");
}

TEST_F(RouterComprehensiveTest2, Regex_Hyphen) {
  router_.add_regex_route(HttpMethod::GET, "/slug/([a-z0-9-]+)", {"slug"},
                          [](const HttpRequest::ptr &req, HttpResponse &resp) {
                            resp.text(req->path_param("slug"));
                          });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/slug/my-cool-post-123"), resp);
  EXPECT_EQ(resp.body_content(), "my-cool-post-123");
}

TEST_F(RouterComprehensiveTest2, Regex_LongPrefix) {
  router_.add_regex_route(
      HttpMethod::GET, "/api/v1/organizations/123/teams/456/members/(\\d+)",
      {"id"}, [](const HttpRequest::ptr &req, HttpResponse &resp) {
        resp.text(req->path_param("id"));
      });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET,
                             "/api/v1/organizations/123/teams/456/members/789"),
                resp);
  EXPECT_EQ(resp.body_content(), "789");
}

TEST_F(RouterComprehensiveTest2, Regex_MethodNotMatch) {
  router_.add_regex_route(
      HttpMethod::GET, "/only-get/(\\d+)", {"id"},
      [](const HttpRequest::ptr &, HttpResponse &resp) { resp.text("ok"); });
  HttpResponse resp;
  EXPECT_FALSE(
      router_.route(make_request(HttpMethod::POST, "/only-get/123"), resp));
}

// ===================== 中间件测试 (61-75) =====================

class CounterMiddleware : public Middleware {
public:
  int &counter;
  CounterMiddleware(int &c) : counter(c) {}
  bool before(const HttpRequest::ptr &, HttpResponse &) override {
    counter++;
    return true;
  }
  void after(const HttpRequest::ptr &, HttpResponse &) override {
    counter += 10;
  }
};

class BlockingMiddleware : public Middleware {
public:
  bool before(const HttpRequest::ptr &, HttpResponse &resp) override {
    resp.text("blocked");
    return false;
  }
  void after(const HttpRequest::ptr &, HttpResponse &) override {}
};

class HeaderMiddleware : public Middleware {
public:
  std::string key, value;
  HeaderMiddleware(const std::string &k, const std::string &v)
      : key(k), value(v) {}
  bool before(const HttpRequest::ptr &, HttpResponse &resp) override {
    resp.header(key, value);
    return true;
  }
  void after(const HttpRequest::ptr &, HttpResponse &) override {}
};

TEST_F(RouterComprehensiveTest2, Middleware_GlobalBefore) {
  int counter = 0;
  router_.use(std::make_shared<CounterMiddleware>(counter));
  router_.get("/test", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("ok");
  });

  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/test"), resp);
  EXPECT_EQ(counter, 11); // before(1) + after(10)
}

TEST_F(RouterComprehensiveTest2, Middleware_GlobalMultiple) {
  int counter = 0;
  router_.use(std::make_shared<CounterMiddleware>(counter));
  router_.use(std::make_shared<CounterMiddleware>(counter));
  router_.get("/test", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("ok");
  });

  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/test"), resp);
  EXPECT_EQ(counter, 22); // 2 * (before + after)
}

TEST_F(RouterComprehensiveTest2, Middleware_Blocking) {
  router_.use(std::make_shared<BlockingMiddleware>());
  router_.get("/test", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("handler");
  });

  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/test"), resp);
  EXPECT_EQ(resp.body_content(), "blocked");
}

TEST_F(RouterComprehensiveTest2, Middleware_AddHeader) {
  router_.use(std::make_shared<HeaderMiddleware>("X-Custom", "Value"));
  router_.get("/test", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("ok");
  });

  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/test"), resp);
  auto it = resp.headers().find("X-Custom");
  EXPECT_NE(it, resp.headers().end());
  if (it != resp.headers().end()) {
    EXPECT_EQ(it->second, "Value");
  }
}

TEST_F(RouterComprehensiveTest2, Middleware_ExecutionOrder) {
  std::string order;

  class OrderMiddleware : public Middleware {
  public:
    std::string &order;
    std::string id;
    OrderMiddleware(std::string &o, std::string i) : order(o), id(i) {}
    bool before(const HttpRequest::ptr &, HttpResponse &) override {
      order += id + "-before;";
      return true;
    }
    void after(const HttpRequest::ptr &, HttpResponse &) override {
      order += id + "-after;";
    }
  };

  router_.use(std::make_shared<OrderMiddleware>(order, "A"));
  router_.use(std::make_shared<OrderMiddleware>(order, "B"));
  router_.get("/test", [&order](const HttpRequest::ptr &, HttpResponse &resp) {
    order += "handler;";
    resp.text("ok");
  });

  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/test"), resp);
  EXPECT_EQ(order, "A-before;B-before;handler;B-after;A-after;");
}

TEST_F(RouterComprehensiveTest2, Middleware_NotFoundStillExecutes) {
  int counter = 0;
  router_.use(std::make_shared<CounterMiddleware>(counter));

  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/notexist"), resp);
  EXPECT_EQ(counter, 11);
}

TEST_F(RouterComprehensiveTest2, Middleware_WithDynamicRoute) {
  int counter = 0;
  router_.use(std::make_shared<CounterMiddleware>(counter));
  router_.get("/users/:id",
              [](const HttpRequest::ptr &req, HttpResponse &resp) {
                resp.text(req->path_param("id"));
              });

  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/users/123"), resp);
  EXPECT_EQ(counter, 11);
  EXPECT_EQ(resp.body_content(), "123");
}

TEST_F(RouterComprehensiveTest2, Middleware_WithRegexRoute) {
  int counter = 0;
  router_.use(std::make_shared<CounterMiddleware>(counter));
  router_.add_regex_route(HttpMethod::GET, "/items/(\\d+)", {"id"},
                          [](const HttpRequest::ptr &req, HttpResponse &resp) {
                            resp.text(req->path_param("id"));
                          });

  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/items/456"), resp);
  EXPECT_EQ(counter, 11);
  EXPECT_EQ(resp.body_content(), "456");
}

// ===================== 边界测试 (76-90) =====================

TEST_F(RouterComprehensiveTest2, Edge_EmptyPath) {
  router_.get("", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("empty");
  });
  HttpResponse resp;
  // Empty path typically maps to root
  router_.route(make_request(HttpMethod::GET, ""), resp);
}

TEST_F(RouterComprehensiveTest2, Edge_OnlySlash) {
  router_.get("/", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("root");
  });
  HttpResponse resp;
  EXPECT_TRUE(router_.route(make_request(HttpMethod::GET, "/"), resp));
}

TEST_F(RouterComprehensiveTest2, Edge_DoubleSlash) {
  router_.get("/a//b", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("double");
  });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/a//b"), resp);
}

TEST_F(RouterComprehensiveTest2, Edge_VeryLongPath) {
  std::string longPath = "/";
  for (int i = 0; i < 50; ++i) {
    longPath += "segment" + std::to_string(i) + "/";
  }
  router_.get(longPath, [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("long");
  });
  HttpResponse resp;
  EXPECT_TRUE(router_.route(make_request(HttpMethod::GET, longPath), resp));
}

TEST_F(RouterComprehensiveTest2, Edge_SingleCharPath) {
  router_.get("/a", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("a");
  });
  HttpResponse resp;
  EXPECT_TRUE(router_.route(make_request(HttpMethod::GET, "/a"), resp));
}

TEST_F(RouterComprehensiveTest2, Edge_NumericPath) {
  router_.get("/123", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("num");
  });
  HttpResponse resp;
  EXPECT_TRUE(router_.route(make_request(HttpMethod::GET, "/123"), resp));
}

TEST_F(RouterComprehensiveTest2, Edge_DotInPath) {
  router_.get("/file.json", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("json");
  });
  HttpResponse resp;
  EXPECT_TRUE(router_.route(make_request(HttpMethod::GET, "/file.json"), resp));
}

TEST_F(RouterComprehensiveTest2, Edge_MultipleTrailingSlashes) {
  router_.get("/path///", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("ok");
  });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/path///"), resp);
}

TEST_F(RouterComprehensiveTest2, Edge_SpecialChars) {
  router_.get("/path@special#chars",
              [](const HttpRequest::ptr &, HttpResponse &resp) {
                resp.text("special");
              });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/path@special#chars"), resp);
}

TEST_F(RouterComprehensiveTest2, Edge_EncodedSlash) {
  router_.get("/path%2Fwith%2Fslashes",
              [](const HttpRequest::ptr &, HttpResponse &resp) {
                resp.text("encoded");
              });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/path%2Fwith%2Fslashes"), resp);
}

// ===================== RouteHandler类测试 (91-100) =====================

class SimpleHandler : public RouteHandler {
public:
  void handle(const HttpRequest::ptr &, HttpResponse &resp) override {
    resp.text("handler-class");
  }
};

class ParamHandler : public RouteHandler {
public:
  void handle(const HttpRequest::ptr &req, HttpResponse &resp) override {
    resp.text("id=" + req->path_param("id"));
  }
};

TEST_F(RouterComprehensiveTest2, Handler_StaticRoute) {
  router_.get("/handler", std::make_shared<SimpleHandler>());
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/handler"), resp);
  EXPECT_EQ(resp.body_content(), "handler-class");
}

TEST_F(RouterComprehensiveTest2, Handler_DynamicRoute) {
  router_.get("/users/:id", std::make_shared<ParamHandler>());
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/users/42"), resp);
  EXPECT_EQ(resp.body_content(), "id=42");
}

TEST_F(RouterComprehensiveTest2, Handler_PostRoute) {
  router_.post("/submit", std::make_shared<SimpleHandler>());
  HttpResponse resp;
  router_.route(make_request(HttpMethod::POST, "/submit"), resp);
  EXPECT_EQ(resp.body_content(), "handler-class");
}

TEST_F(RouterComprehensiveTest2, Handler_PutRoute) {
  router_.put("/update/:id", std::make_shared<ParamHandler>());
  HttpResponse resp;
  router_.route(make_request(HttpMethod::PUT, "/update/99"), resp);
  EXPECT_EQ(resp.body_content(), "id=99");
}

TEST_F(RouterComprehensiveTest2, Handler_DeleteRoute) {
  router_.del("/remove/:id", std::make_shared<ParamHandler>());
  HttpResponse resp;
  router_.route(make_request(HttpMethod::DELETE, "/remove/77"), resp);
  EXPECT_EQ(resp.body_content(), "id=77");
}

TEST_F(RouterComprehensiveTest2, Handler_MixedWithCallback) {
  router_.get("/callback", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("callback");
  });
  router_.get("/handler", std::make_shared<SimpleHandler>());

  HttpResponse resp1, resp2;
  router_.route(make_request(HttpMethod::GET, "/callback"), resp1);
  router_.route(make_request(HttpMethod::GET, "/handler"), resp2);
  EXPECT_EQ(resp1.body_content(), "callback");
  EXPECT_EQ(resp2.body_content(), "handler-class");
}

TEST_F(RouterComprehensiveTest2, Handler_NotFoundHandler) {
  class Custom404Handler : public RouteHandler {
  public:
    void handle(const HttpRequest::ptr &, HttpResponse &resp) override {
      resp.status(HttpStatus::NOT_FOUND).text("custom-404");
    }
  };

  router_.set_not_found_handler(std::make_shared<Custom404Handler>());
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/nonexistent"), resp);
  EXPECT_EQ(resp.body_content(), "custom-404");
}

TEST_F(RouterComprehensiveTest2, Handler_WithMiddleware) {
  int counter = 0;
  router_.use(std::make_shared<CounterMiddleware>(counter));
  router_.get("/handler", std::make_shared<SimpleHandler>());

  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/handler"), resp);
  EXPECT_EQ(counter, 11);
  EXPECT_EQ(resp.body_content(), "handler-class");
}

TEST_F(RouterComprehensiveTest2, Handler_RegexRoute) {
  class RegexHandler : public RouteHandler {
  public:
    void handle(const HttpRequest::ptr &req, HttpResponse &resp) override {
      resp.text("regex-" + req->path_param("id"));
    }
  };

  router_.add_regex_route(HttpMethod::GET, "/items/(\\d+)", {"id"},
                          std::make_shared<RegexHandler>());
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/items/789"), resp);
  EXPECT_EQ(resp.body_content(), "regex-789");
}

TEST_F(RouterComprehensiveTest2, Handler_SharedAcrossRoutes) {
  auto handler = std::make_shared<SimpleHandler>();
  router_.get("/route1", handler);
  router_.get("/route2", handler);

  HttpResponse resp1, resp2;
  router_.route(make_request(HttpMethod::GET, "/route1"), resp1);
  router_.route(make_request(HttpMethod::GET, "/route2"), resp2);
  EXPECT_EQ(resp1.body_content(), "handler-class");
  EXPECT_EQ(resp2.body_content(), "handler-class");
}

int main(int argc, char **argv) {
  init_logger(zlog::LogLevel::value::WARNING);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
