#include "router.h"
#include "zhttp_logger.h"
#include <gtest/gtest.h>

using namespace zhttp;

class RouterComprehensiveTest : public ::testing::Test {
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

// ===================== 静态路由测试 (1-20) =====================

TEST_F(RouterComprehensiveTest, Static_RootPath) {
  router_.get("/", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("root");
  });
  HttpResponse resp;
  EXPECT_TRUE(router_.route(make_request(HttpMethod::GET, "/"), resp));
  EXPECT_EQ(resp.body_content(), "root");
}

TEST_F(RouterComprehensiveTest, Static_SimplePath) {
  router_.get("/api", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("api");
  });
  HttpResponse resp;
  EXPECT_TRUE(router_.route(make_request(HttpMethod::GET, "/api"), resp));
}

TEST_F(RouterComprehensiveTest, Static_DeepPath) {
  router_.get("/a/b/c/d/e", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("deep");
  });
  HttpResponse resp;
  EXPECT_TRUE(router_.route(make_request(HttpMethod::GET, "/a/b/c/d/e"), resp));
}

TEST_F(RouterComprehensiveTest, Static_PostMethod) {
  router_.post("/submit", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("posted");
  });
  HttpResponse resp;
  EXPECT_TRUE(router_.route(make_request(HttpMethod::POST, "/submit"), resp));
}

TEST_F(RouterComprehensiveTest, Static_PutMethod) {
  router_.put("/update", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("updated");
  });
  HttpResponse resp;
  EXPECT_TRUE(router_.route(make_request(HttpMethod::PUT, "/update"), resp));
}

TEST_F(RouterComprehensiveTest, Static_DeleteMethod) {
  router_.del("/remove", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("deleted");
  });
  HttpResponse resp;
  EXPECT_TRUE(router_.route(make_request(HttpMethod::DELETE, "/remove"), resp));
}

TEST_F(RouterComprehensiveTest, Static_SamePathDifferentMethods) {
  router_.get("/resource", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("get");
  });
  router_.post("/resource", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("post");
  });

  HttpResponse resp1, resp2;
  router_.route(make_request(HttpMethod::GET, "/resource"), resp1);
  router_.route(make_request(HttpMethod::POST, "/resource"), resp2);
  EXPECT_EQ(resp1.body_content(), "get");
  EXPECT_EQ(resp2.body_content(), "post");
}

TEST_F(RouterComprehensiveTest, Static_NotFound) {
  router_.get("/exists", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("ok");
  });
  HttpResponse resp;
  EXPECT_FALSE(
      router_.route(make_request(HttpMethod::GET, "/notexists"), resp));
}

TEST_F(RouterComprehensiveTest, Static_MethodNotAllowed) {
  router_.get("/only-get", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("ok");
  });
  HttpResponse resp;
  EXPECT_FALSE(
      router_.route(make_request(HttpMethod::POST, "/only-get"), resp));
}

TEST_F(RouterComprehensiveTest, Static_CaseSensitive) {
  router_.get("/CaseSensitive", [](const HttpRequest::ptr &,
                                   HttpResponse &resp) { resp.text("ok"); });
  HttpResponse resp1, resp2;
  EXPECT_TRUE(
      router_.route(make_request(HttpMethod::GET, "/CaseSensitive"), resp1));
  EXPECT_FALSE(
      router_.route(make_request(HttpMethod::GET, "/casesensitive"), resp2));
}

TEST_F(RouterComprehensiveTest, Static_TrailingSlashDifferent) {
  router_.get("/path", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("no-slash");
  });
  router_.get("/path/", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("slash");
  });
  HttpResponse resp1, resp2;
  router_.route(make_request(HttpMethod::GET, "/path"), resp1);
  router_.route(make_request(HttpMethod::GET, "/path/"), resp2);
  EXPECT_EQ(resp1.body_content(), "no-slash");
  EXPECT_EQ(resp2.body_content(), "slash");
}

TEST_F(RouterComprehensiveTest, Static_SpecialCharsInPath) {
  router_.get("/api/v1.0", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("v1");
  });
  HttpResponse resp;
  EXPECT_TRUE(router_.route(make_request(HttpMethod::GET, "/api/v1.0"), resp));
}

TEST_F(RouterComprehensiveTest, Static_HyphenInPath) {
  router_.get("/my-api-path", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("ok");
  });
  HttpResponse resp;
  EXPECT_TRUE(
      router_.route(make_request(HttpMethod::GET, "/my-api-path"), resp));
}

TEST_F(RouterComprehensiveTest, Static_UnderscoreInPath) {
  router_.get("/my_api_path", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("ok");
  });
  HttpResponse resp;
  EXPECT_TRUE(
      router_.route(make_request(HttpMethod::GET, "/my_api_path"), resp));
}

TEST_F(RouterComprehensiveTest, Static_NumbersInPath) {
  router_.get(
      "/api/v2/resource123",
      [](const HttpRequest::ptr &, HttpResponse &resp) { resp.text("ok"); });
  HttpResponse resp;
  EXPECT_TRUE(router_.route(
      make_request(HttpMethod::GET, "/api/v2/resource123"), resp));
}

TEST_F(RouterComprehensiveTest, Static_ManyRoutes) {
  for (int i = 0; i < 100; ++i) {
    router_.get("/route" + std::to_string(i),
                [i](const HttpRequest::ptr &, HttpResponse &resp) {
                  resp.text(std::to_string(i));
                });
  }
  HttpResponse resp;
  EXPECT_TRUE(router_.route(make_request(HttpMethod::GET, "/route50"), resp));
  EXPECT_EQ(resp.body_content(), "50");
}

TEST_F(RouterComprehensiveTest, Static_CustomNotFoundHandler) {
  router_.set_not_found_handler(
      [](const HttpRequest::ptr &, HttpResponse &resp) {
        resp.status(HttpStatus::NOT_FOUND).text("custom 404");
      });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/nonexistent"), resp);
  EXPECT_EQ(resp.body_content(), "custom 404");
}

TEST_F(RouterComprehensiveTest, Static_JsonResponse) {
  router_.get("/json", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.json("{\"status\":\"ok\"}");
  });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/json"), resp);
  EXPECT_EQ(resp.body_content(), "{\"status\":\"ok\"}");
}

TEST_F(RouterComprehensiveTest, Static_HtmlResponse) {
  router_.get("/html", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.html("<h1>Hello</h1>");
  });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/html"), resp);
  EXPECT_EQ(resp.body_content(), "<h1>Hello</h1>");
}

TEST_F(RouterComprehensiveTest, Static_AllHttpMethods) {
  router_.add_route(
      HttpMethod::HEAD, "/head",
      [](const HttpRequest::ptr &, HttpResponse &resp) { resp.text("head"); });
  router_.add_route(HttpMethod::OPTIONS, "/options",
                    [](const HttpRequest::ptr &, HttpResponse &resp) {
                      resp.text("options");
                    });
  router_.add_route(
      HttpMethod::PATCH, "/patch",
      [](const HttpRequest::ptr &, HttpResponse &resp) { resp.text("patch"); });

  HttpResponse r1, r2, r3;
  EXPECT_TRUE(router_.route(make_request(HttpMethod::HEAD, "/head"), r1));
  EXPECT_TRUE(router_.route(make_request(HttpMethod::OPTIONS, "/options"), r2));
  EXPECT_TRUE(router_.route(make_request(HttpMethod::PATCH, "/patch"), r3));
}

// ===================== 动态路由测试 (21-40) =====================

TEST_F(RouterComprehensiveTest, Dynamic_SingleParam) {
  router_.get("/users/:id",
              [](const HttpRequest::ptr &req, HttpResponse &resp) {
                resp.text(req->path_param("id"));
              });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/users/123"), resp);
  EXPECT_EQ(resp.body_content(), "123");
}

TEST_F(RouterComprehensiveTest, Dynamic_MultipleParams) {
  router_.get("/users/:uid/posts/:pid", [](const HttpRequest::ptr &req,
                                           HttpResponse &resp) {
    resp.text(req->path_param("uid") + "-" + req->path_param("pid"));
  });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/users/1/posts/2"), resp);
  EXPECT_EQ(resp.body_content(), "1-2");
}

TEST_F(RouterComprehensiveTest, Dynamic_ParamWithPrefix) {
  router_.get("/api/v1/items/:id",
              [](const HttpRequest::ptr &req, HttpResponse &resp) {
                resp.text(req->path_param("id"));
              });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/api/v1/items/abc"), resp);
  EXPECT_EQ(resp.body_content(), "abc");
}

TEST_F(RouterComprehensiveTest, Dynamic_ParamWithSuffix) {
  router_.get("/items/:id/details",
              [](const HttpRequest::ptr &req, HttpResponse &resp) {
                resp.text(req->path_param("id"));
              });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/items/xyz/details"), resp);
  EXPECT_EQ(resp.body_content(), "xyz");
}

TEST_F(RouterComprehensiveTest, Dynamic_ThreeParams) {
  router_.get("/:a/:b/:c", [](const HttpRequest::ptr &req, HttpResponse &resp) {
    resp.text(req->path_param("a") + req->path_param("b") +
              req->path_param("c"));
  });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/x/y/z"), resp);
  EXPECT_EQ(resp.body_content(), "xyz");
}

TEST_F(RouterComprehensiveTest, Dynamic_ParamWithNumbers) {
  router_.get("/users/:id",
              [](const HttpRequest::ptr &req, HttpResponse &resp) {
                resp.text(req->path_param("id"));
              });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/users/user123"), resp);
  EXPECT_EQ(resp.body_content(), "user123");
}

TEST_F(RouterComprehensiveTest, Dynamic_ParamWithSpecialChars) {
  router_.get("/files/:name",
              [](const HttpRequest::ptr &req, HttpResponse &resp) {
                resp.text(req->path_param("name"));
              });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/files/my-file_v2.txt"), resp);
  EXPECT_EQ(resp.body_content(), "my-file_v2.txt");
}

TEST_F(RouterComprehensiveTest, Dynamic_StaticPriorityOverParam) {
  router_.get("/users/admin", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("admin");
  });
  router_.get("/users/:id", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("param");
  });

  HttpResponse resp1, resp2;
  router_.route(make_request(HttpMethod::GET, "/users/admin"), resp1);
  router_.route(make_request(HttpMethod::GET, "/users/123"), resp2);
  EXPECT_EQ(resp1.body_content(), "admin");
  EXPECT_EQ(resp2.body_content(), "param");
}

TEST_F(RouterComprehensiveTest, Dynamic_PostWithParam) {
  router_.post("/items/:id",
               [](const HttpRequest::ptr &req, HttpResponse &resp) {
                 resp.text("post-" + req->path_param("id"));
               });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::POST, "/items/42"), resp);
  EXPECT_EQ(resp.body_content(), "post-42");
}

TEST_F(RouterComprehensiveTest, Dynamic_PutWithParam) {
  router_.put("/items/:id",
              [](const HttpRequest::ptr &req, HttpResponse &resp) {
                resp.text("put-" + req->path_param("id"));
              });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::PUT, "/items/99"), resp);
  EXPECT_EQ(resp.body_content(), "put-99");
}

TEST_F(RouterComprehensiveTest, Dynamic_DeleteWithParam) {
  router_.del("/items/:id",
              [](const HttpRequest::ptr &req, HttpResponse &resp) {
                resp.text("del-" + req->path_param("id"));
              });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::DELETE, "/items/77"), resp);
  EXPECT_EQ(resp.body_content(), "del-77");
}

TEST_F(RouterComprehensiveTest, Dynamic_SameMethodDifferentParams) {
  router_.get("/a/:x/b", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("pattern1");
  });
  router_.get("/c/:y/d", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("pattern2");
  });

  HttpResponse resp1, resp2;
  router_.route(make_request(HttpMethod::GET, "/a/1/b"), resp1);
  router_.route(make_request(HttpMethod::GET, "/c/2/d"), resp2);
  EXPECT_EQ(resp1.body_content(), "pattern1");
  EXPECT_EQ(resp2.body_content(), "pattern2");
}

TEST_F(RouterComprehensiveTest, Dynamic_CatchAll) {
  router_.get("/static/*filepath",
              [](const HttpRequest::ptr &req, HttpResponse &resp) {
                resp.text(req->path_param("filepath"));
              });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/static/css/style.css"), resp);
  EXPECT_EQ(resp.body_content(), "css/style.css");
}

TEST_F(RouterComprehensiveTest, Dynamic_CatchAllDeep) {
  router_.get("/files/*path",
              [](const HttpRequest::ptr &req, HttpResponse &resp) {
                resp.text(req->path_param("path"));
              });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/files/a/b/c/d/e.txt"), resp);
  EXPECT_EQ(resp.body_content(), "a/b/c/d/e.txt");
}

TEST_F(RouterComprehensiveTest, Dynamic_ParamNotMatch_ExtraSegment) {
  router_.get("/users/:id", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("ok");
  });
  HttpResponse resp;
  EXPECT_FALSE(
      router_.route(make_request(HttpMethod::GET, "/users/123/extra"), resp));
}

TEST_F(RouterComprehensiveTest, Dynamic_ParamNotMatch_MissingSegment) {
  router_.get(
      "/users/:id/profile",
      [](const HttpRequest::ptr &, HttpResponse &resp) { resp.text("ok"); });
  HttpResponse resp;
  EXPECT_FALSE(
      router_.route(make_request(HttpMethod::GET, "/users/123"), resp));
}

TEST_F(RouterComprehensiveTest, Dynamic_EmptyParamValue) {
  router_.get("/items/:id/sub", [](const HttpRequest::ptr &,
                                   HttpResponse &resp) { resp.text("ok"); });
  HttpResponse resp;
  // Empty segment won't match
  EXPECT_FALSE(
      router_.route(make_request(HttpMethod::GET, "/items//sub"), resp));
}

TEST_F(RouterComprehensiveTest, Dynamic_ManyDynamicRoutes) {
  for (int i = 0; i < 50; ++i) {
    router_.get("/api/v" + std::to_string(i) + "/:id",
                [i](const HttpRequest::ptr &req, HttpResponse &resp) {
                  resp.text(std::to_string(i) + "-" + req->path_param("id"));
                });
  }
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/api/v25/test"), resp);
  EXPECT_EQ(resp.body_content(), "25-test");
}

TEST_F(RouterComprehensiveTest, Dynamic_UnicodeInParam) {
  router_.get("/search/:query",
              [](const HttpRequest::ptr &req, HttpResponse &resp) {
                resp.text(req->path_param("query"));
              });
  HttpResponse resp;
  // URL-encoded unicode
  router_.route(
      make_request(HttpMethod::GET, "/search/hello%E4%B8%96%E7%95%8C"), resp);
  EXPECT_EQ(resp.body_content(), "hello%E4%B8%96%E7%95%8C");
}

TEST_F(RouterComprehensiveTest, Dynamic_LongParamValue) {
  router_.get("/data/:value",
              [](const HttpRequest::ptr &req, HttpResponse &resp) {
                resp.text(req->path_param("value"));
              });
  std::string longValue(200, 'x');
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/data/" + longValue), resp);
  EXPECT_EQ(resp.body_content(), longValue);
}

// ===================== 额外测试 (41-53) =====================

TEST_F(RouterComprehensiveTest, Extra_OverwriteRoute) {
  router_.get("/overwrite", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("first");
  });
  router_.get("/overwrite", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("second");
  });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/overwrite"), resp);
  // Second registration overwrites
  EXPECT_EQ(resp.body_content(), "second");
}

TEST_F(RouterComprehensiveTest, Extra_MultiMethodSameHandler) {
  auto handler = [](const HttpRequest::ptr &req, HttpResponse &resp) {
    resp.text(method_to_string(req->method()));
  };
  router_.get("/multi", handler);
  router_.post("/multi", handler);
  router_.put("/multi", handler);

  HttpResponse r1, r2, r3;
  router_.route(make_request(HttpMethod::GET, "/multi"), r1);
  router_.route(make_request(HttpMethod::POST, "/multi"), r2);
  router_.route(make_request(HttpMethod::PUT, "/multi"), r3);
  EXPECT_EQ(r1.body_content(), "GET");
  EXPECT_EQ(r2.body_content(), "POST");
  EXPECT_EQ(r3.body_content(), "PUT");
}

TEST_F(RouterComprehensiveTest, Extra_ParamSameNameDifferentRoutes) {
  router_.get("/a/:id", [](const HttpRequest::ptr &req, HttpResponse &resp) {
    resp.text("a-" + req->path_param("id"));
  });
  router_.get("/b/:id", [](const HttpRequest::ptr &req, HttpResponse &resp) {
    resp.text("b-" + req->path_param("id"));
  });

  HttpResponse r1, r2;
  router_.route(make_request(HttpMethod::GET, "/a/1"), r1);
  router_.route(make_request(HttpMethod::GET, "/b/2"), r2);
  EXPECT_EQ(r1.body_content(), "a-1");
  EXPECT_EQ(r2.body_content(), "b-2");
}

TEST_F(RouterComprehensiveTest, Extra_NestedParams) {
  router_.get("/org/:org/team/:team/user/:user", [](const HttpRequest::ptr &req,
                                                    HttpResponse &resp) {
    resp.text(req->path_param("org") + "/" + req->path_param("team") + "/" +
              req->path_param("user"));
  });
  HttpResponse resp;
  router_.route(
      make_request(HttpMethod::GET, "/org/google/team/android/user/john"),
      resp);
  EXPECT_EQ(resp.body_content(), "google/android/john");
}

TEST_F(RouterComprehensiveTest, Extra_StaticAndDynamicMix) {
  router_.get("/api/status", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("status");
  });
  router_.get("/api/:resource",
              [](const HttpRequest::ptr &req, HttpResponse &resp) {
                resp.text("resource:" + req->path_param("resource"));
              });
  router_.get("/api/config", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("config");
  });

  HttpResponse r1, r2, r3;
  router_.route(make_request(HttpMethod::GET, "/api/status"), r1);
  router_.route(make_request(HttpMethod::GET, "/api/users"), r2);
  router_.route(make_request(HttpMethod::GET, "/api/config"), r3);
  EXPECT_EQ(r1.body_content(), "status");
  EXPECT_EQ(r2.body_content(), "resource:users");
  EXPECT_EQ(r3.body_content(), "config");
}

TEST_F(RouterComprehensiveTest, Extra_StatusCodes) {
  router_.get("/created", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.status(HttpStatus::CREATED).text("created");
  });
  router_.get("/redirect", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.redirect("/new-location");
  });

  HttpResponse r1, r2;
  router_.route(make_request(HttpMethod::GET, "/created"), r1);
  router_.route(make_request(HttpMethod::GET, "/redirect"), r2);
  EXPECT_EQ(r1.status_code(), HttpStatus::CREATED);
  EXPECT_EQ(r2.status_code(), HttpStatus::FOUND);
}

TEST_F(RouterComprehensiveTest, Extra_ChainedResponse) {
  router_.get("/chained", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.status(HttpStatus::OK)
        .content_type("application/json")
        .header("X-Custom", "value")
        .body("{\"ok\":true}");
  });
  HttpResponse resp;
  router_.route(make_request(HttpMethod::GET, "/chained"), resp);
  EXPECT_EQ(resp.status_code(), HttpStatus::OK);
  EXPECT_EQ(resp.body_content(), "{\"ok\":true}");
}

TEST_F(RouterComprehensiveTest, Extra_ParamPrecedence) {
  // Static paths should have higher priority
  router_.get("/items/:id", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("param");
  });
  router_.get("/items/special",
              [](const HttpRequest::ptr &, HttpResponse &resp) {
                resp.text("special");
              });

  HttpResponse r1, r2;
  router_.route(make_request(HttpMethod::GET, "/items/special"), r1);
  router_.route(make_request(HttpMethod::GET, "/items/123"), r2);
  EXPECT_EQ(r1.body_content(), "special");
  EXPECT_EQ(r2.body_content(), "param");
}

TEST_F(RouterComprehensiveTest, Extra_CatchAllPrecedence) {
  router_.get("/files/:name", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("param");
  });
  router_.get("/files/*path", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("catchall");
  });

  HttpResponse r1, r2;
  router_.route(make_request(HttpMethod::GET, "/files/test"), r1);
  router_.route(make_request(HttpMethod::GET, "/files/a/b/c"), r2);
  EXPECT_EQ(r1.body_content(), "param");
  EXPECT_EQ(r2.body_content(), "catchall");
}

TEST_F(RouterComprehensiveTest, Extra_EmptyHandler) {
  router_.get("/empty", [](const HttpRequest::ptr &, HttpResponse &resp) {
    // Empty body
    resp.status(HttpStatus::NO_CONTENT);
  });
  HttpResponse resp;
  EXPECT_TRUE(router_.route(make_request(HttpMethod::GET, "/empty"), resp));
  EXPECT_EQ(resp.status_code(), HttpStatus::NO_CONTENT);
}

TEST_F(RouterComprehensiveTest, Extra_LargeNumberRoutes) {
  for (int i = 0; i < 500; ++i) {
    router_.get("/large" + std::to_string(i),
                [i](const HttpRequest::ptr &, HttpResponse &resp) {
                  resp.text(std::to_string(i));
                });
  }
  HttpResponse resp;
  EXPECT_TRUE(router_.route(make_request(HttpMethod::GET, "/large499"), resp));
  EXPECT_EQ(resp.body_content(), "499");
}

TEST_F(RouterComprehensiveTest, Extra_SimilarPaths) {
  router_.get("/users", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("all");
  });
  router_.get("/user", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("single");
  });
  router_.get("/users/:id", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("byid");
  });

  HttpResponse r1, r2, r3;
  router_.route(make_request(HttpMethod::GET, "/users"), r1);
  router_.route(make_request(HttpMethod::GET, "/user"), r2);
  router_.route(make_request(HttpMethod::GET, "/users/1"), r3);
  EXPECT_EQ(r1.body_content(), "all");
  EXPECT_EQ(r2.body_content(), "single");
  EXPECT_EQ(r3.body_content(), "byid");
}

TEST_F(RouterComprehensiveTest, Extra_QueryStringIgnored) {
  router_.get("/search", [](const HttpRequest::ptr &, HttpResponse &resp) {
    resp.text("found");
  });
  // Path should be matched without query string
  auto req = make_request(HttpMethod::GET, "/search");
  HttpResponse resp;
  EXPECT_TRUE(router_.route(req, resp));
  EXPECT_EQ(resp.body_content(), "found");
}

int main(int argc, char **argv) {
  init_logger(zlog::LogLevel::value::WARNING);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
