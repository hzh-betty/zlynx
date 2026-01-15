#include "router.h"
#include "zhttp_logger.h"
#include <gtest/gtest.h>

class RegexPrefixBucketTest : public ::testing::Test {
protected:
  void SetUp() override { zhttp::init_logger(); }
};

// 测试正则路由前缀分桶
TEST_F(RegexPrefixBucketTest, RegexPrefixGrouping) {
  zhttp::Router router;

  // 注册多个正则路由，按前缀分桶
  // 前缀 /api/v1/users/
  router.add_regex_route(
      zhttp::HttpMethod::GET, "/api/v1/users/(\\d+)", {"user_id"},
      [](const zhttp::HttpRequest::ptr &req, zhttp::HttpResponse &resp) {
        resp.json("{\"user_id\": \"" + req->path_param("user_id") + "\"}");
      });

  router.add_regex_route(
      zhttp::HttpMethod::GET, "/api/v1/users/(\\d+)/profile", {"user_id"},
      [](const zhttp::HttpRequest::ptr &req, zhttp::HttpResponse &resp) {
        resp.json("{\"user_id\": \"" + req->path_param("user_id") +
                  "\", \"type\": \"profile\"}");
      });

  // 前缀 /api/v1/photos/
  router.add_regex_route(
      zhttp::HttpMethod::GET, "/api/v1/photos/(\\d+)", {"photo_id"},
      [](const zhttp::HttpRequest::ptr &req, zhttp::HttpResponse &resp) {
        resp.json("{\"photo_id\": \"" + req->path_param("photo_id") + "\"}");
      });

  // 前缀 /api/v2/
  router.add_regex_route(
      zhttp::HttpMethod::GET, "/api/v2/items/([a-z]+)-(\\d+)", {"type", "id"},
      [](const zhttp::HttpRequest::ptr &req, zhttp::HttpResponse &resp) {
        resp.json("{\"type\": \"" + req->path_param("type") + "\", \"id\": \"" +
                  req->path_param("id") + "\"}");
      });

  // 测试匹配
  auto req1 = std::make_shared<zhttp::HttpRequest>();
  req1->set_method(zhttp::HttpMethod::GET);
  req1->set_path("/api/v1/users/12345");
  zhttp::HttpResponse resp1;
  EXPECT_TRUE(router.route(req1, resp1));
  EXPECT_EQ(req1->path_param("user_id"), "12345");

  auto req2 = std::make_shared<zhttp::HttpRequest>();
  req2->set_method(zhttp::HttpMethod::GET);
  req2->set_path("/api/v1/users/999/profile");
  zhttp::HttpResponse resp2;
  EXPECT_TRUE(router.route(req2, resp2));
  EXPECT_EQ(req2->path_param("user_id"), "999");

  auto req3 = std::make_shared<zhttp::HttpRequest>();
  req3->set_method(zhttp::HttpMethod::GET);
  req3->set_path("/api/v1/photos/42");
  zhttp::HttpResponse resp3;
  EXPECT_TRUE(router.route(req3, resp3));
  EXPECT_EQ(req3->path_param("photo_id"), "42");

  auto req4 = std::make_shared<zhttp::HttpRequest>();
  req4->set_method(zhttp::HttpMethod::GET);
  req4->set_path("/api/v2/items/book-123");
  zhttp::HttpResponse resp4;
  EXPECT_TRUE(router.route(req4, resp4));
  EXPECT_EQ(req4->path_param("type"), "book");
  EXPECT_EQ(req4->path_param("id"), "123");
}

// 测试动态路由优先于正则路由
TEST_F(RegexPrefixBucketTest, DynamicRouteHasPriority) {
  zhttp::Router router;

  // 动态路由
  router.add_route(zhttp::HttpMethod::GET, "/users/:id",
                   [](const zhttp::HttpRequest::ptr &,
                      zhttp::HttpResponse &resp) { resp.text("dynamic"); });

  // 正则路由（同一路径）
  router.add_regex_route(zhttp::HttpMethod::GET, "/users/(\\d+)", {"id"},
                         [](const zhttp::HttpRequest::ptr &,
                            zhttp::HttpResponse &resp) { resp.text("regex"); });

  // 动态路由应该优先匹配
  auto req = std::make_shared<zhttp::HttpRequest>();
  req->set_method(zhttp::HttpMethod::GET);
  req->set_path("/users/123");
  zhttp::HttpResponse resp;
  EXPECT_TRUE(router.route(req, resp));
  EXPECT_EQ(resp.body_content(), "dynamic");
}

// 测试正则路由不匹配时的行为
TEST_F(RegexPrefixBucketTest, RegexNoMatch) {
  zhttp::Router router;

  router.add_regex_route(
      zhttp::HttpMethod::GET, "/api/v1/users/(\\d+)", {"id"},
      [](const zhttp::HttpRequest::ptr &, zhttp::HttpResponse &resp) {
        resp.text("matched");
      });

  // 不匹配的路径（字母而非数字）
  auto req = std::make_shared<zhttp::HttpRequest>();
  req->set_method(zhttp::HttpMethod::GET);
  req->set_path("/api/v1/users/abc");
  zhttp::HttpResponse resp;
  EXPECT_FALSE(router.route(req, resp));
}

// 测试大量正则路由的性能
TEST_F(RegexPrefixBucketTest, ManyRegexRoutes) {
  zhttp::Router router;

  // 注册100个正则路由，分布在10个不同前缀
  for (int prefix = 0; prefix < 10; ++prefix) {
    for (int route = 0; route < 10; ++route) {
      std::string pattern = "/api/v" + std::to_string(prefix) + "/resource" +
                            std::to_string(route) + "/(\\d+)";
      router.add_regex_route(
          zhttp::HttpMethod::GET, pattern, {"id"},
          [](const zhttp::HttpRequest::ptr &, zhttp::HttpResponse &resp) {
            resp.text("ok");
          });
    }
  }

  // 测试匹配 - 应该只在对应前缀的桶内搜索
  auto req = std::make_shared<zhttp::HttpRequest>();
  req->set_method(zhttp::HttpMethod::GET);
  req->set_path("/api/v5/resource7/12345");
  zhttp::HttpResponse resp;
  EXPECT_TRUE(router.route(req, resp));
  EXPECT_EQ(req->path_param("id"), "12345");
}

int main(int argc, char **argv) {
  zhttp::init_logger();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
