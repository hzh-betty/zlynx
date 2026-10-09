#include "../support/request_builder.h"
#include "zhttp/router/router.h"
#include <gtest/gtest.h>

// 测试正则路由前缀分桶
TEST(RegexPrefixBucketTest, RegexPrefixGrouping) {
    zhttp::HttpApplication router;

    // 注册多个正则路由，按前缀分桶
    // 前缀 /api/v1/users/
    router.router().add_regex_route(zhttp::HttpMethod::GET, "/api/v1/users/(\\d+)",
                           {"user_id"}, [](zhttp::HttpContext &context) {
                               auto *req = &context;
                               auto &resp = context.response();
                               resp.json("{\"user_id\": \"" +
                                         req->path_param("user_id") + "\"}");
                           });

    router.router().add_regex_route(
        zhttp::HttpMethod::GET, "/api/v1/users/(\\d+)/profile", {"user_id"},
        [](zhttp::HttpContext &context) {
            auto *req = &context;
            auto &resp = context.response();
            resp.json("{\"user_id\": \"" + req->path_param("user_id") +
                      "\", \"type\": \"profile\"}");
        });

    // 前缀 /api/v1/photos/
    router.router().add_regex_route(zhttp::HttpMethod::GET, "/api/v1/photos/(\\d+)",
                           {"photo_id"}, [](zhttp::HttpContext &context) {
                               auto *req = &context;
                               auto &resp = context.response();
                               resp.json("{\"photo_id\": \"" +
                                         req->path_param("photo_id") + "\"}");
                           });

    // 前缀 /api/v2/
    router.router().add_regex_route(
        zhttp::HttpMethod::GET, "/api/v2/items/([a-z]+)-(\\d+)", {"type", "id"},
        [](zhttp::HttpContext &context) {
            auto *req = &context;
            auto &resp = context.response();
            resp.json("{\"type\": \"" + req->path_param("type") +
                      "\", \"id\": \"" + req->path_param("id") + "\"}");
        });

    // 测试匹配
    auto req1 = std::make_shared<zhttp::TestContext>();
    req1->set_method(zhttp::HttpMethod::GET);
    req1->set_path("/api/v1/users/12345");
    zhttp::HttpResponse resp1;
    EXPECT_TRUE(run_application(router, req1, resp1));
    EXPECT_EQ(req1->path_param("user_id"), "12345");

    auto req2 = std::make_shared<zhttp::TestContext>();
    req2->set_method(zhttp::HttpMethod::GET);
    req2->set_path("/api/v1/users/999/profile");
    zhttp::HttpResponse resp2;
    EXPECT_TRUE(run_application(router, req2, resp2));
    EXPECT_EQ(req2->path_param("user_id"), "999");

    auto req3 = std::make_shared<zhttp::TestContext>();
    req3->set_method(zhttp::HttpMethod::GET);
    req3->set_path("/api/v1/photos/42");
    zhttp::HttpResponse resp3;
    EXPECT_TRUE(run_application(router, req3, resp3));
    EXPECT_EQ(req3->path_param("photo_id"), "42");

    auto req4 = std::make_shared<zhttp::TestContext>();
    req4->set_method(zhttp::HttpMethod::GET);
    req4->set_path("/api/v2/items/book-123");
    zhttp::HttpResponse resp4;
    EXPECT_TRUE(run_application(router, req4, resp4));
    EXPECT_EQ(req4->path_param("type"), "book");
    EXPECT_EQ(req4->path_param("id"), "123");
}

// 测试动态路由优先于正则路由
TEST(RegexPrefixBucketTest, DynamicRouteHasPriority) {
    zhttp::HttpApplication router;

    // 动态路由
    router.router().add_route(zhttp::HttpMethod::GET, "/users/:id",
                     [](zhttp::HttpContext &context) {
                         auto &resp = context.response();
                         resp.text("dynamic");
                     });

    // 正则路由（同一路径）
    router.router().add_regex_route(zhttp::HttpMethod::GET, "/users/(\\d+)", {"id"},
                           [](zhttp::HttpContext &context) {
                               auto &resp = context.response();
                               resp.text("regex");
                           });

    // 动态路由应该优先匹配
    auto req = std::make_shared<zhttp::TestContext>();
    req->set_method(zhttp::HttpMethod::GET);
    req->set_path("/users/123");
    zhttp::HttpResponse resp;
    EXPECT_TRUE(run_application(router, req, resp));
    EXPECT_EQ(resp.body_content(), "dynamic");
}

// 测试正则路由不匹配时的行为
TEST(RegexPrefixBucketTest, RegexNoMatch) {
    zhttp::HttpApplication router;

    router.router().add_regex_route(zhttp::HttpMethod::GET, "/api/v1/users/(\\d+)",
                           {"id"}, [](zhttp::HttpContext &context) {
                               auto &resp = context.response();
                               resp.text("matched");
                           });

    // 不匹配的路径（字母而非数字）
    auto req = std::make_shared<zhttp::TestContext>();
    req->set_method(zhttp::HttpMethod::GET);
    req->set_path("/api/v1/users/abc");
    zhttp::HttpResponse resp;
    EXPECT_FALSE(run_application(router, req, resp));
}

// 测试大量正则路由的性能
TEST(RegexPrefixBucketTest, ManyRegexRoutes) {
    zhttp::HttpApplication router;

    // 注册100个正则路由，分布在10个不同前缀
    for (int prefix = 0; prefix < 10; ++prefix) {
        for (int route = 0; route < 10; ++route) {
            std::string pattern = "/api/v" + std::to_string(prefix) +
                                  "/resource" + std::to_string(route) +
                                  "/(\\d+)";
            router.router().add_regex_route(zhttp::HttpMethod::GET, pattern, {"id"},
                                   [](zhttp::HttpContext &context) {
                                       auto &resp = context.response();
                                       resp.text("ok");
                                   });
        }
    }

    // 测试匹配 - 应该只在对应前缀的桶内搜索
    auto req = std::make_shared<zhttp::TestContext>();
    req->set_method(zhttp::HttpMethod::GET);
    req->set_path("/api/v5/resource7/12345");
    zhttp::HttpResponse resp;
    EXPECT_TRUE(run_application(router, req, resp));
    EXPECT_EQ(req->path_param("id"), "12345");
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);

    return RUN_ALL_TESTS();
}
