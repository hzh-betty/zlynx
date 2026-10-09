#include "zhttp/http_application.h"
#include <gtest/gtest.h>
#include <thread>

using namespace zhttp;

TEST(HttpApplicationTest, ExecutesOfflineAndLeavesCompletionToCaller) {
    HttpApplication app;
    app.router().get("/items/:id", [](HttpContext &context) {
        context.response().text(context.path_param("id"));
    });
    app.freeze();
    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    request->set_path("/items/42");
    HttpContext context(request);
    int completions = 0;
    context.on_complete([&](CompletionResult) { ++completions; });
    EXPECT_TRUE(app.handle(context));
    EXPECT_EQ(context.response().body_content(), "42");
    EXPECT_FALSE(context.response().committed());
    EXPECT_EQ(completions, 0);
    context.complete(CompletionResult::Completed);
    EXPECT_EQ(completions, 1);
    EXPECT_THROW(app.router().get("/late", [](HttpContext &) {}), std::logic_error);
    EXPECT_THROW(app.use(nullptr), std::logic_error);
    EXPECT_THROW(app.set_exception_handler({}), std::logic_error);
}

TEST(HttpApplicationTest, FrozenApplicationHandlesIndependentRequestsConcurrently) {
    HttpApplication app;
    app.router().get("/:value", [](HttpContext &context) {
        context.response().text(context.path_param("value"));
    });
    app.freeze();
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&, i] {
            auto request = std::make_shared<HttpRequest>();
            request->set_method(HttpMethod::GET);
            request->set_path("/" + std::to_string(i));
            HttpContext context(request);
            EXPECT_TRUE(app.handle(context));
            EXPECT_EQ(context.response().body_content(), std::to_string(i));
        });
    }
    for (auto &thread : threads)
        thread.join();
}

TEST(RouterOwnershipTest, SharedDynamicTopologyKeepsEachRoutesParameterNames) {
    Router router;
    router.get("/users/:id", [](HttpContext &) {});
    router.post("/users/:name", [](HttpContext &) {});
    router.get("/users/:user/posts/:post", [](HttpContext &) {});
    router.get("/users/:account/files/*file", [](HttpContext &) {});
    EXPECT_EQ(router.match("/users/42", HttpMethod::GET).params,
              (HttpRequest::Params{{"id", "42"}}));
    EXPECT_EQ(router.match("/users/alice", HttpMethod::POST).params,
              (HttpRequest::Params{{"name", "alice"}}));
    EXPECT_EQ(router.match("/users/42/posts/7", HttpMethod::GET).params,
              (HttpRequest::Params{{"user", "42"}, {"post", "7"}}));
    EXPECT_EQ(router.match("/users/42/files/a/b", HttpMethod::GET).params,
              (HttpRequest::Params{{"account", "42"}, {"file", "a/b"}}));
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

TEST(RouterOwnershipTest, RegexRoutesKeepParameterNamesPerMethod) {
    Router router;
    router.add_regex_route(HttpMethod::GET, "/files/(.*)", {"path"}, [](HttpContext &) {});
    router.add_regex_route(HttpMethod::POST, "/files/(.*)", {"name"}, [](HttpContext &) {});
    EXPECT_EQ(router.match("/files/readme", HttpMethod::GET).params,
              (HttpRequest::Params{{"path", "readme"}}));
    EXPECT_EQ(router.match("/files/readme", HttpMethod::POST).params,
              (HttpRequest::Params{{"name", "readme"}}));
}

TEST(RouterOwnershipTest, DuplicateDynamicParameterNamesKeepFirstValue) {
    Router router;
    router.get("/:id/again/:id", [](HttpContext &) {});
    EXPECT_EQ(router.match("/first/again/second", HttpMethod::GET).params,
              (HttpRequest::Params{{"id", "first"}}));
}
