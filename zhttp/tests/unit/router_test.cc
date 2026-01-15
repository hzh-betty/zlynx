#include "router.h"
#include "zhttp_logger.h"

#include <gtest/gtest.h>

using namespace zhttp;

class RouterTest : public ::testing::Test {
protected:
  Router router_;
};

TEST_F(RouterTest, StaticRouteMatch) {
  bool handler_called = false;
  router_.get("/api/users",
              [&handler_called](const HttpRequest::ptr &, HttpResponse &resp) {
                handler_called = true;
                resp.status(HttpStatus::OK).text("users");
              });

  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_path("/api/users");

  HttpResponse response;
  bool found = router_.route(request, response);

  EXPECT_TRUE(found);
  EXPECT_TRUE(handler_called);
  EXPECT_EQ(response.body_content(), "users");
}

TEST_F(RouterTest, ParamRouteMatch) {
  std::string captured_id;
  router_.get("/users/:id",
              [&captured_id](const HttpRequest::ptr &req, HttpResponse &resp) {
                captured_id = req->path_param("id");
                resp.status(HttpStatus::OK);
              });

  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_path("/users/123");

  HttpResponse response;
  bool found = router_.route(request, response);

  EXPECT_TRUE(found);
  EXPECT_EQ(captured_id, "123");
}

TEST_F(RouterTest, MultipleParamRoute) {
  std::string captured_user_id, captured_post_id;
  router_.get("/users/:user_id/posts/:post_id",
              [&captured_user_id,
               &captured_post_id](const HttpRequest::ptr &req, HttpResponse &) {
                captured_user_id = req->path_param("user_id");
                captured_post_id = req->path_param("post_id");
              });

  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_path("/users/42/posts/99");

  HttpResponse response;
  router_.route(request, response);

  EXPECT_EQ(captured_user_id, "42");
  EXPECT_EQ(captured_post_id, "99");
}

TEST_F(RouterTest, RegexRouteMatch) {
  std::string captured_version;
  router_.add_regex_route(
      HttpMethod::GET, "^/api/v(\\d+)/users$", {"version"},
      [&captured_version](const HttpRequest::ptr &req, HttpResponse &) {
        captured_version = req->path_param("version");
      });

  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_path("/api/v2/users");

  HttpResponse response;
  bool found = router_.route(request, response);

  EXPECT_TRUE(found);
  EXPECT_EQ(captured_version, "2");
}

TEST_F(RouterTest, NotFoundHandler) {
  bool not_found_called = false;
  router_.set_not_found_handler(
      [&not_found_called](const HttpRequest::ptr &, HttpResponse &resp) {
        not_found_called = true;
        resp.status(HttpStatus::NOT_FOUND).text("Custom 404");
      });

  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_path("/nonexistent");

  HttpResponse response;
  bool found = router_.route(request, response);

  EXPECT_FALSE(found);
  EXPECT_TRUE(not_found_called);
  EXPECT_EQ(response.status_code(), HttpStatus::NOT_FOUND);
}

TEST_F(RouterTest, DifferentMethodsSamePath) {
  std::string method_called;
  router_.get("/resource",
              [&method_called](const HttpRequest::ptr &, HttpResponse &) {
                method_called = "GET";
              });
  router_.post("/resource",
               [&method_called](const HttpRequest::ptr &, HttpResponse &) {
                 method_called = "POST";
               });

  auto get_request = std::make_shared<HttpRequest>();
  get_request->set_method(HttpMethod::GET);
  get_request->set_path("/resource");
  HttpResponse get_response;
  router_.route(get_request, get_response);
  EXPECT_EQ(method_called, "GET");

  auto post_request = std::make_shared<HttpRequest>();
  post_request->set_method(HttpMethod::POST);
  post_request->set_path("/resource");
  HttpResponse post_response;
  router_.route(post_request, post_response);
  EXPECT_EQ(method_called, "POST");
}

TEST_F(RouterTest, StaticRoutePriorityOverParam) {
  std::string matched;
  router_.get("/users/admin",
              [&matched](const HttpRequest::ptr &, HttpResponse &) {
                matched = "static";
              });
  router_.get("/users/:id", [&matched](const HttpRequest::ptr &,
                                       HttpResponse &) { matched = "param"; });

  // 静态路由应该优先匹配
  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::GET);
  request->set_path("/users/admin");
  HttpResponse response;
  router_.route(request, response);

  EXPECT_EQ(matched, "static");

  // 其他路径应该匹配参数路由
  request->set_path("/users/123");
  router_.route(request, response);
  EXPECT_EQ(matched, "param");
}

int main(int argc, char **argv) {
  zhttp::init_logger();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
