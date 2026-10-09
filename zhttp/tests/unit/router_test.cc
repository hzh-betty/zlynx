#include "../support/request_builder.h"
#include "zhttp/router/router.h"

#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <vector>

using namespace zhttp;
using namespace zhttp::mid;

class RouterTest : public ::testing::Test {
  protected:
    HttpApplication router_;
};

namespace {

TestContext::ptr make_request(HttpMethod method, const std::string &path) {
    auto req = std::make_shared<TestContext>();
    req->set_method(method);
    req->set_path(path);
    return req;
}

class SimpleRouteHandler : public RouteHandler {
  public:
    void handle(HttpContext &context) override {
        auto &resp = context.response();
        resp.text("handler-class");
    }
};

class ParamRouteHandler : public RouteHandler {
  public:
    void handle(HttpContext &context) override {
        auto *req = &context;
        auto &resp = context.response();
        resp.text("id=" + req->path_param("id"));
    }
};

} // namespace

TEST_F(RouterTest, StaticRouteMatch) {
    bool handler_called = false;
    router_.router().get("/api/users", [&handler_called](HttpContext &context) {
        auto &resp = context.response();
        handler_called = true;
        resp.status(HttpStatus::OK).text("users");
    });

    auto request = std::make_shared<TestContext>();
    request->set_method(HttpMethod::GET);
    request->set_path("/api/users");

    HttpResponse response;
    bool found = run_application(router_, request, response);

    EXPECT_TRUE(found);
    EXPECT_TRUE(handler_called);
    EXPECT_EQ(response.body_content(), "users");
}

TEST_F(RouterTest, HomepageRedirectsRootAndHome) {
    router_.router().set_homepage("dashboard");

    auto root_request = std::make_shared<TestContext>();
    root_request->set_method(HttpMethod::GET);
    root_request->set_path("/");

    HttpResponse root_response;
    EXPECT_TRUE(run_application(router_, root_request, root_response));
    EXPECT_EQ(root_response.status_code(), HttpStatus::FOUND);
    EXPECT_EQ(root_response.headers().at("Location"), "/dashboard");

    auto home_request = std::make_shared<TestContext>();
    home_request->set_method(HttpMethod::GET);
    home_request->set_path("/home");

    HttpResponse home_response;
    EXPECT_TRUE(run_application(router_, home_request, home_response));
    EXPECT_EQ(home_response.status_code(), HttpStatus::FOUND);
    EXPECT_EQ(home_response.headers().at("Location"), "/dashboard");
}

TEST_F(RouterTest, HomepageRedirectsHeadButNotPost) {
    router_.router().set_homepage("dashboard");

    HttpResponse head_response;
    EXPECT_TRUE(
        run_application(router_, make_request(HttpMethod::HEAD, "/"), head_response));
    EXPECT_EQ(head_response.status_code(), HttpStatus::FOUND);
    EXPECT_EQ(head_response.headers().at("Location"), "/dashboard");

    HttpResponse post_response;
    EXPECT_FALSE(
        run_application(router_, make_request(HttpMethod::POST, "/"), post_response));
    EXPECT_EQ(post_response.status_code(), HttpStatus::NOT_FOUND);
}

TEST_F(RouterTest, ParamRouteMatch) {
    std::string captured_id;
    router_.router().get("/users/:id", [&captured_id](HttpContext &context) {
        auto *req = &context;
        auto &resp = context.response();
        captured_id = req->path_param("id");
        resp.status(HttpStatus::OK);
    });

    auto request = std::make_shared<TestContext>();
    request->set_method(HttpMethod::GET);
    request->set_path("/users/123");

    HttpResponse response;
    bool found = run_application(router_, request, response);

    EXPECT_TRUE(found);
    EXPECT_EQ(captured_id, "123");
}

TEST_F(RouterTest, MultipleParamRoute) {
    std::string captured_user_id, captured_post_id;
    router_.router().get("/users/:user_id/posts/:post_id",
                [&captured_user_id, &captured_post_id](HttpContext &context) {
                    auto *req = &context;
                    captured_user_id = req->path_param("user_id");
                    captured_post_id = req->path_param("post_id");
                });

    auto request = std::make_shared<TestContext>();
    request->set_method(HttpMethod::GET);
    request->set_path("/users/42/posts/99");

    HttpResponse response;
    run_application(router_, request, response);

    EXPECT_EQ(captured_user_id, "42");
    EXPECT_EQ(captured_post_id, "99");
}

TEST_F(RouterTest, CatchAllRouteMatch) {
    std::string captured_path;
    router_.router().get("/static/*filepath", [&captured_path](HttpContext &context) {
        auto *req = &context;
        auto &resp = context.response();
        captured_path = req->path_param("filepath");
        resp.status(HttpStatus::OK);
    });

    HttpResponse response;
    bool found = run_application(router_,
        make_request(HttpMethod::GET, "/static/css/style.css"), response);
    EXPECT_TRUE(found);
    EXPECT_EQ(captured_path, "css/style.css");
}

TEST_F(RouterTest, RegexRouteMatch) {
    std::string captured_version;
    router_.router().add_regex_route(HttpMethod::GET, "^/api/v(\\d+)/users$",
                            {"version"},
                            [&captured_version](HttpContext &context) {
                                auto *req = &context;
                                captured_version = req->path_param("version");
                            });

    auto request = std::make_shared<TestContext>();
    request->set_method(HttpMethod::GET);
    request->set_path("/api/v2/users");

    HttpResponse response;
    bool found = run_application(router_, request, response);

    EXPECT_TRUE(found);
    EXPECT_EQ(captured_version, "2");
}

TEST_F(RouterTest, RegexRouteMethodNotMatch) {
    router_.router().add_regex_route(HttpMethod::GET, "/only-get/(\\d+)", {"id"},
                            [](HttpContext &context) {
                                auto &resp = context.response();
                                resp.text("ok");
                            });

    HttpResponse response;
    bool found = run_application(router_, make_request(HttpMethod::POST, "/only-get/123"),
                               response);
    EXPECT_FALSE(found);
}

TEST_F(RouterTest, NotFoundHandler) {
    bool not_found_called = false;
    router_.set_not_found_handler([&not_found_called](HttpContext &context) {
        auto &resp = context.response();
        not_found_called = true;
        resp.status(HttpStatus::NOT_FOUND).text("Custom 404");
    });

    auto request = std::make_shared<TestContext>();
    request->set_method(HttpMethod::GET);
    request->set_path("/nonexistent");

    HttpResponse response;
    bool found = run_application(router_, request, response);

    EXPECT_FALSE(found);
    EXPECT_TRUE(not_found_called);
    EXPECT_EQ(response.status_code(), HttpStatus::NOT_FOUND);
}

TEST_F(RouterTest, NotFoundHandlerWithRouteHandlerPtr) {
    class Custom404Handler final : public RouteHandler {
      public:
        void handle(HttpContext &context) override {
            auto &resp = context.response();
            resp.status(HttpStatus::NOT_FOUND).text("custom-404");
        }
    };

    router_.set_not_found_handler(make_route_callback(std::make_shared<Custom404Handler>()));
    HttpResponse response;
    bool found =
        run_application(router_, make_request(HttpMethod::GET, "/missing"), response);
    EXPECT_FALSE(found);
    EXPECT_EQ(response.status_code(), HttpStatus::NOT_FOUND);
    EXPECT_EQ(response.body_content(), "custom-404");
}

TEST_F(RouterTest, DifferentMethodsSamePath) {
    std::string method_called;
    router_.router().get("/resource", [&method_called](HttpContext &context) {
        method_called = "GET";
    });
    router_.router().post("/resource", [&method_called](HttpContext &context) {
        method_called = "POST";
    });

    auto get_request = std::make_shared<TestContext>();
    get_request->set_method(HttpMethod::GET);
    get_request->set_path("/resource");
    HttpResponse get_response;
    run_application(router_, get_request, get_response);
    EXPECT_EQ(method_called, "GET");

    auto post_request = std::make_shared<TestContext>();
    post_request->set_method(HttpMethod::POST);
    post_request->set_path("/resource");
    HttpResponse post_response;
    run_application(router_, post_request, post_response);
    EXPECT_EQ(method_called, "POST");
}

TEST_F(RouterTest, SupportsAdditionalHttpMethods) {
    router_.router().add_route(HttpMethod::HEAD, "/head", [](HttpContext &context) {
        auto &resp = context.response();
        resp.text("head");
    });
    router_.router().add_route(HttpMethod::OPTIONS, "/options",
                      [](HttpContext &context) {
                          auto &resp = context.response();
                          resp.text("options");
                      });
    router_.router().add_route(HttpMethod::PATCH, "/patch", [](HttpContext &context) {
        auto &resp = context.response();
        resp.text("patch");
    });

    HttpResponse head_resp;
    HttpResponse options_resp;
    HttpResponse patch_resp;
    EXPECT_TRUE(
        run_application(router_, make_request(HttpMethod::HEAD, "/head"), head_resp));
    EXPECT_TRUE(run_application(router_, make_request(HttpMethod::OPTIONS, "/options"),
                              options_resp));
    EXPECT_TRUE(
        run_application(router_, make_request(HttpMethod::PATCH, "/patch"), patch_resp));
    EXPECT_EQ(head_resp.body_content(), "head");
    EXPECT_EQ(options_resp.body_content(), "options");
    EXPECT_EQ(patch_resp.body_content(), "patch");
}

TEST_F(RouterTest, StaticRoutePriorityOverParam) {
    std::string matched;
    router_.router().get("/users/admin",
                [&matched](HttpContext &context) { matched = "static"; });
    router_.router().get("/users/:id",
                [&matched](HttpContext &context) { matched = "param"; });

    // 静态路由应该优先匹配
    auto request = std::make_shared<TestContext>();
    request->set_method(HttpMethod::GET);
    request->set_path("/users/admin");
    HttpResponse response;
    run_application(router_, request, response);

    EXPECT_EQ(matched, "static");

    // 其他路径应该匹配参数路由
    request->set_path("/users/123");
    run_application(router_, request, response);
    EXPECT_EQ(matched, "param");
}

TEST_F(RouterTest, RouteHandlerPtrOverloadsWorkAcrossMethods) {
    router_.router().get("/handler", std::make_shared<SimpleRouteHandler>());
    router_.router().post("/submit", std::make_shared<SimpleRouteHandler>());
    router_.router().put("/update/:id", std::make_shared<ParamRouteHandler>());
    router_.router().del("/remove/:id", std::make_shared<ParamRouteHandler>());

    HttpResponse r1;
    HttpResponse r2;
    HttpResponse r3;
    HttpResponse r4;

    EXPECT_TRUE(run_application(router_, make_request(HttpMethod::GET, "/handler"), r1));
    EXPECT_TRUE(run_application(router_, make_request(HttpMethod::POST, "/submit"), r2));
    EXPECT_TRUE(run_application(router_, make_request(HttpMethod::PUT, "/update/99"), r3));
    EXPECT_TRUE(
        run_application(router_, make_request(HttpMethod::DELETE, "/remove/77"), r4));

    EXPECT_EQ(r1.body_content(), "handler-class");
    EXPECT_EQ(r2.body_content(), "handler-class");
    EXPECT_EQ(r3.body_content(), "id=99");
    EXPECT_EQ(r4.body_content(), "id=77");
}

TEST_F(RouterTest, RegexRouteWithRouteHandlerPtr) {
    class RegexHandler : public RouteHandler {
      public:
        void handle(HttpContext &context) override {
            auto *req = &context;
            auto &resp = context.response();
            resp.text("regex-" + req->path_param("id"));
        }
    };

    router_.router().add_regex_route(HttpMethod::GET, "/items/(\\d+)", {"id"},
                            std::make_shared<RegexHandler>());

    HttpResponse response;
    EXPECT_TRUE(
        run_application(router_, make_request(HttpMethod::GET, "/items/789"), response));
    EXPECT_EQ(response.body_content(), "regex-789");
}

TEST_F(RouterTest, HandlerExceptionReturnsInternalServerError) {
    router_.router().get("/panic", [](HttpContext &context) {
        throw std::runtime_error("handler boom");
    });

    auto request = std::make_shared<TestContext>();
    request->set_method(HttpMethod::GET);
    request->set_path("/panic");

    HttpResponse response;
    bool found = run_application(router_, request, response);

    EXPECT_TRUE(found);
    EXPECT_EQ(response.status_code(), HttpStatus::INTERNAL_SERVER_ERROR);
    EXPECT_EQ(response.body_content(), "Internal Server Error");
}

TEST_F(RouterTest, NonStdHandlerExceptionReturnsInternalServerError) {
    router_.router().get("/panic-non-std", [](HttpContext &context) { throw 42; });

    HttpResponse response;
    bool found = run_application(router_, make_request(HttpMethod::GET, "/panic-non-std"),
                               response);
    EXPECT_TRUE(found);
    EXPECT_EQ(response.status_code(), HttpStatus::INTERNAL_SERVER_ERROR);
}

TEST_F(RouterTest, MiddlewareBeforeExceptionReturnsInternalServerError) {
    class ThrowBeforeMiddleware : public Middleware {
      public:
        bool before(HttpContext &context) override {

            throw std::runtime_error("before boom");
        }

        void after(HttpContext &context) override {}
    };

    bool handler_called = false;
    router_.use(std::make_shared<ThrowBeforeMiddleware>());
    router_.router().get("/mw-before", [&handler_called](HttpContext &context) {
        auto &resp = context.response();
        handler_called = true;
        resp.status(HttpStatus::OK).text("ok");
    });

    auto request = std::make_shared<TestContext>();
    request->set_method(HttpMethod::GET);
    request->set_path("/mw-before");

    HttpResponse response;
    bool found = run_application(router_, request, response);

    EXPECT_TRUE(found);
    EXPECT_FALSE(handler_called);
    EXPECT_EQ(response.status_code(), HttpStatus::INTERNAL_SERVER_ERROR);
}

TEST_F(RouterTest, MiddlewareBeforeReturnsFalseSkipsHandlerButExecutesAfter) {
    class BlockingMiddleware : public Middleware {
      public:
        bool before(HttpContext &context) override {
            auto &resp = context.response();
            resp.status(HttpStatus::UNAUTHORIZED).text("blocked");
            return false;
        }
        void after(HttpContext &context) override {}
    };
    class AfterFlagMiddleware : public Middleware {
      public:
        explicit AfterFlagMiddleware(bool &called) : called_(called) {}
        bool before(HttpContext &context) override { return true; }
        void after(HttpContext &context) override { called_ = true; }

      private:
        bool &called_;
    };

    bool handler_called = false;
    bool after_called = false;
    router_.use(std::make_shared<AfterFlagMiddleware>(after_called));
    router_.use(std::make_shared<BlockingMiddleware>());
    router_.router().get("/blocked", [&handler_called](HttpContext &context) {
        handler_called = true;
    });

    HttpResponse response;
    bool found =
        run_application(router_, make_request(HttpMethod::GET, "/blocked"), response);
    EXPECT_TRUE(found);
    EXPECT_FALSE(handler_called);
    EXPECT_TRUE(after_called);
    EXPECT_EQ(response.status_code(), HttpStatus::UNAUTHORIZED);
    EXPECT_EQ(response.body_content(), "blocked");
}

TEST_F(RouterTest, MiddlewareAfterExceptionReturnsInternalServerError) {
    class ThrowAfterMiddleware : public Middleware {
      public:
        bool before(HttpContext &context) override { return true; }

        void after(HttpContext &context) override {

            throw std::runtime_error("after boom");
        }
    };

    router_.use(std::make_shared<ThrowAfterMiddleware>());
    router_.router().get("/mw-after", [](HttpContext &context) {
        auto &resp = context.response();
        resp.status(HttpStatus::OK).text("ok");
    });

    auto request = std::make_shared<TestContext>();
    request->set_method(HttpMethod::GET);
    request->set_path("/mw-after");

    HttpResponse response;
    bool found = run_application(router_, request, response);

    EXPECT_TRUE(found);
    EXPECT_EQ(response.status_code(), HttpStatus::INTERNAL_SERVER_ERROR);
}

TEST_F(RouterTest, ExceptionHandlerThrowFallsBackToInternalServerError) {
    router_.set_exception_handler([](HttpContext &context, std::exception_ptr) {
        throw std::runtime_error("exception-handler-boom");
    });
    router_.router().get("/eh-throw",
                [](HttpContext &context) { throw std::runtime_error("boom"); });

    HttpResponse response;
    bool found =
        run_application(router_, make_request(HttpMethod::GET, "/eh-throw"), response);
    EXPECT_TRUE(found);
    EXPECT_EQ(response.status_code(), HttpStatus::INTERNAL_SERVER_ERROR);
    EXPECT_EQ(response.body_content(), "Internal Server Error");
}

TEST_F(RouterTest, CustomExceptionHandlerOverridesDefaultResponse) {
    bool exception_handler_called = false;
    std::string captured_path;

    router_.set_exception_handler(
        [&exception_handler_called, &captured_path](HttpContext &context,
                                                    std::exception_ptr) {
            auto *req = &context;
            auto &resp = context.response();
            exception_handler_called = true;
            captured_path = req->path();
            resp.status(HttpStatus::BAD_GATEWAY).json("{\"error\":\"custom\"}");
        });

    router_.router().get("/custom-ex",
                [](HttpContext &context) { throw std::runtime_error("boom"); });

    auto request = std::make_shared<TestContext>();
    request->set_method(HttpMethod::GET);
    request->set_path("/custom-ex");

    HttpResponse response;
    const bool found = run_application(router_, request, response);

    EXPECT_TRUE(found);
    EXPECT_TRUE(exception_handler_called);
    EXPECT_EQ(captured_path, "/custom-ex");
    EXPECT_EQ(response.status_code(), HttpStatus::BAD_GATEWAY);
    EXPECT_EQ(response.body_content(), "{\"error\":\"custom\"}");
}

TEST_F(RouterTest, SetExceptionHandlerNullResetsToDefault) {
    router_.set_exception_handler(
        [&](HttpContext &context, std::exception_ptr) {
            auto &resp = context.response();
            resp.status(HttpStatus::BAD_GATEWAY).text("custom");
        });
    router_.set_exception_handler(nullptr);
    router_.router().get("/default-ex",
                [](HttpContext &context) { throw std::runtime_error("boom"); });

    HttpResponse response;
    bool found =
        run_application(router_, make_request(HttpMethod::GET, "/default-ex"), response);
    EXPECT_TRUE(found);
    EXPECT_EQ(response.status_code(), HttpStatus::INTERNAL_SERVER_ERROR);
    EXPECT_EQ(response.body_content(), "Internal Server Error");
}

TEST_F(RouterTest, ExceptionHandlerThrowNonStdFallsBackToInternalServerError) {
    router_.set_exception_handler(
        [](HttpContext &context, std::exception_ptr) { throw 123; });
    router_.router().get("/eh-non-std-throw",
                [](HttpContext &context) { throw std::runtime_error("boom"); });

    HttpResponse response;
    bool found = run_application(router_,
        make_request(HttpMethod::GET, "/eh-non-std-throw"), response);
    EXPECT_TRUE(found);
    EXPECT_EQ(response.status_code(), HttpStatus::INTERNAL_SERVER_ERROR);
    EXPECT_EQ(response.body_content(), "Internal Server Error");
}

TEST_F(RouterTest, HomepageAliasTargetDoesNotRedirectAliasPath) {
    router_.router().set_homepage("/home");
    HttpResponse response;
    bool found = run_application(router_, make_request(HttpMethod::GET, "/"), response);
    EXPECT_FALSE(found);
    EXPECT_EQ(response.status_code(), HttpStatus::NOT_FOUND);
}

TEST_F(RouterTest, GroupAndPathUseIgnoreNullMiddlewareAndNormalizePrefix) {
    class TraceMiddleware final : public Middleware {
      public:
        explicit TraceMiddleware(std::vector<std::string> &trace)
            : trace_(trace) {}

        bool before(HttpContext &context) override {

            trace_.push_back("before");
            return true;
        }

        void after(HttpContext &context) override { trace_.push_back("after"); }

      private:
        std::vector<std::string> &trace_;
    };

    std::vector<std::string> trace;
    router_.use(nullptr);
    router_.use("/api/users/", nullptr);
    router_.use_group("", std::make_shared<TraceMiddleware>(trace));
    router_.use_group("/", std::make_shared<TraceMiddleware>(trace));
    router_.use_group("/api///", std::make_shared<TraceMiddleware>(trace));
    router_.router().get("/api/users/",
                [&trace](HttpContext &context) { trace.push_back("handler"); });

    HttpResponse response;
    bool found =
        run_application(router_, make_request(HttpMethod::GET, "/api/users/"), response);
    EXPECT_TRUE(found);
    ASSERT_EQ(trace.size(), 3u);
    EXPECT_EQ(trace[0], "before");
    EXPECT_EQ(trace[1], "handler");
    EXPECT_EQ(trace[2], "after");
}

TEST_F(RouterTest, PathMiddlewareRunsForNotFoundWhenPathMatches) {
    class PathOnlyMiddleware final : public Middleware {
      public:
        PathOnlyMiddleware(bool &before_called, bool &after_called)
            : before_called_(before_called), after_called_(after_called) {}

        bool before(HttpContext &context) override {

            before_called_ = true;
            return true;
        }

        void after(HttpContext &context) override { after_called_ = true; }

      private:
        bool &before_called_;
        bool &after_called_;
    };

    bool before_called = false;
    bool after_called = false;
    router_.use("/not-found", std::make_shared<PathOnlyMiddleware>(
                                  before_called, after_called));

    HttpResponse response;
    bool found =
        run_application(router_, make_request(HttpMethod::GET, "/not-found"), response);
    EXPECT_FALSE(found);
    EXPECT_TRUE(before_called);
    EXPECT_TRUE(after_called);
    EXPECT_EQ(response.status_code(), HttpStatus::NOT_FOUND);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);

    return RUN_ALL_TESTS();
}
