#include "request_body_middleware.h"

#include <gtest/gtest.h>

using namespace zhttp;

TEST(RequestBodyMiddlewareTest, RejectInvalidJsonByDefault) {
  RequestBodyMiddleware middleware;

  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::POST);
  request->set_header("Content-Type", "application/json");
  request->set_body("{\"name\":");

  HttpResponse response;
  response.status(HttpStatus::OK).text("ok");

  EXPECT_FALSE(middleware.before(request, response));
  EXPECT_EQ(response.status_code(), HttpStatus::BAD_REQUEST);
}

TEST(RequestBodyMiddlewareTest, ParseValidJsonBeforeHandler) {
  RequestBodyMiddleware middleware;

  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::POST);
  request->set_header("Content-Type", "application/json; charset=utf-8");
  request->set_body("{\"id\":123,\"name\":\"zhttp\"}");

  HttpResponse response;

  EXPECT_TRUE(middleware.before(request, response));
  const HttpRequest::Json *json = request->json();
  ASSERT_NE(json, nullptr);
  EXPECT_EQ((*json)["id"], 123);
  EXPECT_EQ((*json)["name"], "zhttp");
}

TEST(RequestBodyMiddlewareTest, ParseFormUrlencodedBeforeHandler) {
  RequestBodyMiddleware middleware;

  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::POST);
  request->set_header("Content-Type", "application/x-www-form-urlencoded");
  request->set_body("a=1&b=hello+world");

  HttpResponse response;

  EXPECT_TRUE(middleware.before(request, response));
  EXPECT_EQ(request->form_param("a"), "1");
  EXPECT_EQ(request->form_param("b"), "hello world");
}

TEST(RequestBodyMiddlewareTest, RejectInvalidMultipartByDefault) {
  RequestBodyMiddleware middleware;

  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::POST);
  request->set_header("Content-Type", "multipart/form-data");
  request->set_body("--abc--\r\n");

  HttpResponse response;

  EXPECT_FALSE(middleware.before(request, response));
  EXPECT_EQ(response.status_code(), HttpStatus::BAD_REQUEST);
}

TEST(RequestBodyMiddlewareTest, AllowInvalidJsonWhenConfigured) {
  RequestBodyMiddleware::Options options;
  options.reject_invalid_json = false;
  RequestBodyMiddleware middleware(options);

  auto request = std::make_shared<HttpRequest>();
  request->set_method(HttpMethod::POST);
  request->set_header("Content-Type", "application/json");
  request->set_body("{invalid}");

  HttpResponse response;

  EXPECT_TRUE(middleware.before(request, response));
  EXPECT_EQ(request->json(), nullptr);
  EXPECT_FALSE(request->json_error().empty());
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
