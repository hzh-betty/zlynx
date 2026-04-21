#include "zhttp/router.h"
#include "zhttp/zhttp_logger.h"

#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <vector>

using namespace zhttp;
using namespace zhttp::mid;

class RouterTest : public ::testing::Test {
  protected:
    Router router_;
};

namespace {

HttpRequest::ptr make_request(HttpMethod method, const std::string &path) {
    auto req = std::make_shared<HttpRequest>();
    req->set_method(method);
    req->set_path(path);
    return req;
}

class SimpleRouteHandler : public RouteHandler {
  public:
    void handle(const HttpRequest::ptr &, HttpResponse &resp) override {
        resp.text("handler-class");
    }
};

class ParamRouteHandler : public RouteHandler {
  public:
    void handle(const HttpRequest::ptr &req, HttpResponse &resp) override {
        resp.text("id=" + req->path_param("id"));
    }
};

} // namespace

TEST_F(RouterTest, StaticRouteMatch) {
    bool handler_called = false;
    router_.get("/api/users", [&handler_called](const HttpRequest::ptr &,
                                                HttpResponse &resp) {
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

TEST_F(RouterTest, HomepageRedirectsRootAndHome) {
    router_.set_homepage("dashboard");

    auto root_request = std::make_shared<HttpRequest>();
    root_request->set_method(HttpMethod::GET);
    root_request->set_path("/");

    HttpResponse root_response;
    EXPECT_TRUE(router_.route(root_request, root_response));
    EXPECT_EQ(root_response.status_code(), HttpStatus::FOUND);
    EXPECT_EQ(root_response.headers().at("Location"), "/dashboard");

    auto home_request = std::make_shared<HttpRequest>();
    home_request->set_method(HttpMethod::GET);
    home_request->set_path("/home");

    HttpResponse home_response;
    EXPECT_TRUE(router_.route(home_request, home_response));
    EXPECT_EQ(home_response.status_code(), HttpStatus::FOUND);
    EXPECT_EQ(home_response.headers().at("Location"), "/dashboard");
}

TEST_F(RouterTest, HomepageRedirectsHeadButNotPost) {
    router_.set_homepage("dashboard");

    HttpResponse head_response;
    EXPECT_TRUE(
        router_.route(make_request(HttpMethod::HEAD, "/"), head_response));
    EXPECT_EQ(head_response.status_code(), HttpStatus::FOUND);
    EXPECT_EQ(head_response.headers().at("Location"), "/dashboard");

    HttpResponse post_response;
    EXPECT_FALSE(
        router_.route(make_request(HttpMethod::POST, "/"), post_response));
    EXPECT_EQ(post_response.status_code(), HttpStatus::NOT_FOUND);
}

TEST_F(RouterTest, ParamRouteMatch) {
    std::string captured_id;
    router_.get("/users/:id", [&captured_id](const HttpRequest::ptr &req,
                                             HttpResponse &resp) {
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
                [&captured_user_id, &captured_post_id](
                    const HttpRequest::ptr &req, HttpResponse &) {
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

TEST_F(RouterTest, CatchAllRouteMatch) {
    std::string captured_path;
    router_.get(
        "/static/*filepath",
        [&captured_path](const HttpRequest::ptr &req, HttpResponse &resp) {
            captured_path = req->path_param("filepath");
            resp.status(HttpStatus::OK);
        });

    HttpResponse response;
    bool found = router_.route(
        make_request(HttpMethod::GET, "/static/css/style.css"), response);
    EXPECT_TRUE(found);
    EXPECT_EQ(captured_path, "css/style.css");
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

TEST_F(RouterTest, RegexRouteMethodNotMatch) {
    router_.add_regex_route(
        HttpMethod::GET, "/only-get/(\\d+)", {"id"},
        [](const HttpRequest::ptr &, HttpResponse &resp) { resp.text("ok"); });

    HttpResponse response;
    bool found = router_.route(make_request(HttpMethod::POST, "/only-get/123"),
                               response);
    EXPECT_FALSE(found);
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

TEST_F(RouterTest, NotFoundHandlerWithRouteHandlerPtr) {
    class Custom404Handler final : public RouteHandler {
      public:
        void handle(const HttpRequest::ptr &, HttpResponse &resp) override {
            resp.status(HttpStatus::NOT_FOUND).text("custom-404");
        }
    };

    router_.set_not_found_handler(std::make_shared<Custom404Handler>());
    HttpResponse response;
    bool found =
        router_.route(make_request(HttpMethod::GET, "/missing"), response);
    EXPECT_FALSE(found);
    EXPECT_EQ(response.status_code(), HttpStatus::NOT_FOUND);
    EXPECT_EQ(response.body_content(), "custom-404");
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

TEST_F(RouterTest, SupportsAdditionalHttpMethods) {
    router_.add_route(HttpMethod::HEAD, "/head",
                      [](const HttpRequest::ptr &, HttpResponse &resp) {
                          resp.text("head");
                      });
    router_.add_route(HttpMethod::OPTIONS, "/options",
                      [](const HttpRequest::ptr &, HttpResponse &resp) {
                          resp.text("options");
                      });
    router_.add_route(HttpMethod::PATCH, "/patch",
                      [](const HttpRequest::ptr &, HttpResponse &resp) {
                          resp.text("patch");
                      });

    HttpResponse head_resp;
    HttpResponse options_resp;
    HttpResponse patch_resp;
    EXPECT_TRUE(
        router_.route(make_request(HttpMethod::HEAD, "/head"), head_resp));
    EXPECT_TRUE(router_.route(make_request(HttpMethod::OPTIONS, "/options"),
                              options_resp));
    EXPECT_TRUE(
        router_.route(make_request(HttpMethod::PATCH, "/patch"), patch_resp));
    EXPECT_EQ(head_resp.body_content(), "head");
    EXPECT_EQ(options_resp.body_content(), "options");
    EXPECT_EQ(patch_resp.body_content(), "patch");
}

TEST_F(RouterTest, StaticRoutePriorityOverParam) {
    std::string matched;
    router_.get("/users/admin",
                [&matched](const HttpRequest::ptr &, HttpResponse &) {
                    matched = "static";
                });
    router_.get("/users/:id",
                [&matched](const HttpRequest::ptr &, HttpResponse &) {
                    matched = "param";
                });

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

TEST_F(RouterTest, RouteHandlerPtrOverloadsWorkAcrossMethods) {
    router_.get("/handler", std::make_shared<SimpleRouteHandler>());
    router_.post("/submit", std::make_shared<SimpleRouteHandler>());
    router_.put("/update/:id", std::make_shared<ParamRouteHandler>());
    router_.del("/remove/:id", std::make_shared<ParamRouteHandler>());

    HttpResponse r1;
    HttpResponse r2;
    HttpResponse r3;
    HttpResponse r4;

    EXPECT_TRUE(router_.route(make_request(HttpMethod::GET, "/handler"), r1));
    EXPECT_TRUE(router_.route(make_request(HttpMethod::POST, "/submit"), r2));
    EXPECT_TRUE(router_.route(make_request(HttpMethod::PUT, "/update/99"), r3));
    EXPECT_TRUE(
        router_.route(make_request(HttpMethod::DELETE, "/remove/77"), r4));

    EXPECT_EQ(r1.body_content(), "handler-class");
    EXPECT_EQ(r2.body_content(), "handler-class");
    EXPECT_EQ(r3.body_content(), "id=99");
    EXPECT_EQ(r4.body_content(), "id=77");
}

TEST_F(RouterTest, RegexRouteWithRouteHandlerPtr) {
    class RegexHandler : public RouteHandler {
      public:
        void handle(const HttpRequest::ptr &req, HttpResponse &resp) override {
            resp.text("regex-" + req->path_param("id"));
        }
    };

    router_.add_regex_route(HttpMethod::GET, "/items/(\\d+)", {"id"},
                            std::make_shared<RegexHandler>());

    HttpResponse response;
    EXPECT_TRUE(
        router_.route(make_request(HttpMethod::GET, "/items/789"), response));
    EXPECT_EQ(response.body_content(), "regex-789");
}

TEST_F(RouterTest, HandlerExceptionReturnsInternalServerError) {
    router_.get("/panic", [](const HttpRequest::ptr &, HttpResponse &) {
        throw std::runtime_error("handler boom");
    });

    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    request->set_path("/panic");

    HttpResponse response;
    bool found = router_.route(request, response);

    EXPECT_TRUE(found);
    EXPECT_EQ(response.status_code(), HttpStatus::INTERNAL_SERVER_ERROR);
    EXPECT_EQ(response.body_content(), "Internal Server Error");
}

TEST_F(RouterTest, NonStdHandlerExceptionReturnsInternalServerError) {
    router_.get("/panic-non-std",
                [](const HttpRequest::ptr &, HttpResponse &) { throw 42; });

    HttpResponse response;
    bool found = router_.route(make_request(HttpMethod::GET, "/panic-non-std"),
                               response);
    EXPECT_TRUE(found);
    EXPECT_EQ(response.status_code(), HttpStatus::INTERNAL_SERVER_ERROR);
}

TEST_F(RouterTest, MiddlewareBeforeExceptionReturnsInternalServerError) {
    class ThrowBeforeMiddleware : public Middleware {
      public:
        bool before(const HttpRequest::ptr &, HttpResponse &) override {
            throw std::runtime_error("before boom");
        }

        void after(const HttpRequest::ptr &, HttpResponse &) override {}
    };

    bool handler_called = false;
    router_.use(std::make_shared<ThrowBeforeMiddleware>());
    router_.get("/mw-before", [&handler_called](const HttpRequest::ptr &,
                                                HttpResponse &resp) {
        handler_called = true;
        resp.status(HttpStatus::OK).text("ok");
    });

    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    request->set_path("/mw-before");

    HttpResponse response;
    bool found = router_.route(request, response);

    EXPECT_TRUE(found);
    EXPECT_FALSE(handler_called);
    EXPECT_EQ(response.status_code(), HttpStatus::INTERNAL_SERVER_ERROR);
}

TEST_F(RouterTest, MiddlewareBeforeReturnsFalseSkipsHandlerButExecutesAfter) {
    class BlockingMiddleware : public Middleware {
      public:
        bool before(const HttpRequest::ptr &, HttpResponse &resp) override {
            resp.status(HttpStatus::UNAUTHORIZED).text("blocked");
            return false;
        }
        void after(const HttpRequest::ptr &, HttpResponse &) override {}
    };
    class AfterFlagMiddleware : public Middleware {
      public:
        explicit AfterFlagMiddleware(bool &called) : called_(called) {}
        bool before(const HttpRequest::ptr &, HttpResponse &) override {
            return true;
        }
        void after(const HttpRequest::ptr &, HttpResponse &) override {
            called_ = true;
        }

      private:
        bool &called_;
    };

    bool handler_called = false;
    bool after_called = false;
    router_.use(std::make_shared<AfterFlagMiddleware>(after_called));
    router_.use(std::make_shared<BlockingMiddleware>());
    router_.get("/blocked",
                [&handler_called](const HttpRequest::ptr &, HttpResponse &) {
                    handler_called = true;
                });

    HttpResponse response;
    bool found =
        router_.route(make_request(HttpMethod::GET, "/blocked"), response);
    EXPECT_TRUE(found);
    EXPECT_FALSE(handler_called);
    EXPECT_TRUE(after_called);
    EXPECT_EQ(response.status_code(), HttpStatus::UNAUTHORIZED);
    EXPECT_EQ(response.body_content(), "blocked");
}

TEST_F(RouterTest, MiddlewareAfterExceptionReturnsInternalServerError) {
    class ThrowAfterMiddleware : public Middleware {
      public:
        bool before(const HttpRequest::ptr &, HttpResponse &) override {
            return true;
        }

        void after(const HttpRequest::ptr &, HttpResponse &) override {
            throw std::runtime_error("after boom");
        }
    };

    router_.use(std::make_shared<ThrowAfterMiddleware>());
    router_.get("/mw-after", [](const HttpRequest::ptr &, HttpResponse &resp) {
        resp.status(HttpStatus::OK).text("ok");
    });

    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    request->set_path("/mw-after");

    HttpResponse response;
    bool found = router_.route(request, response);

    EXPECT_TRUE(found);
    EXPECT_EQ(response.status_code(), HttpStatus::INTERNAL_SERVER_ERROR);
}

TEST_F(RouterTest, ExceptionHandlerThrowFallsBackToInternalServerError) {
    router_.set_exception_handler(
        [](const HttpRequest::ptr &, HttpResponse &, std::exception_ptr) {
            throw std::runtime_error("exception-handler-boom");
        });
    router_.get("/eh-throw", [](const HttpRequest::ptr &, HttpResponse &) {
        throw std::runtime_error("boom");
    });

    HttpResponse response;
    bool found =
        router_.route(make_request(HttpMethod::GET, "/eh-throw"), response);
    EXPECT_TRUE(found);
    EXPECT_EQ(response.status_code(), HttpStatus::INTERNAL_SERVER_ERROR);
    EXPECT_EQ(response.body_content(), "Internal Server Error");
}

TEST_F(RouterTest, CustomExceptionHandlerOverridesDefaultResponse) {
    bool exception_handler_called = false;
    std::string captured_path;

    router_.set_exception_handler([&exception_handler_called, &captured_path](
                                      const HttpRequest::ptr &req,
                                      HttpResponse &resp, std::exception_ptr) {
        exception_handler_called = true;
        captured_path = req->path();
        resp.status(HttpStatus::BAD_GATEWAY).json("{\"error\":\"custom\"}");
    });

    router_.get("/custom-ex", [](const HttpRequest::ptr &, HttpResponse &) {
        throw std::runtime_error("boom");
    });

    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    request->set_path("/custom-ex");

    HttpResponse response;
    const bool found = router_.route(request, response);

    EXPECT_TRUE(found);
    EXPECT_TRUE(exception_handler_called);
    EXPECT_EQ(captured_path, "/custom-ex");
    EXPECT_EQ(response.status_code(), HttpStatus::BAD_GATEWAY);
    EXPECT_EQ(response.body_content(), "{\"error\":\"custom\"}");
}

TEST_F(RouterTest, SetExceptionHandlerNullResetsToDefault) {
    router_.set_exception_handler(
        [&](const HttpRequest::ptr &, HttpResponse &resp, std::exception_ptr) {
            resp.status(HttpStatus::BAD_GATEWAY).text("custom");
        });
    router_.set_exception_handler(nullptr);
    router_.get("/default-ex", [](const HttpRequest::ptr &, HttpResponse &) {
        throw std::runtime_error("boom");
    });

    HttpResponse response;
    bool found =
        router_.route(make_request(HttpMethod::GET, "/default-ex"), response);
    EXPECT_TRUE(found);
    EXPECT_EQ(response.status_code(), HttpStatus::INTERNAL_SERVER_ERROR);
    EXPECT_EQ(response.body_content(), "Internal Server Error");
}

TEST_F(RouterTest, ExceptionHandlerThrowNonStdFallsBackToInternalServerError) {
    router_.set_exception_handler([](const HttpRequest::ptr &, HttpResponse &,
                                     std::exception_ptr) { throw 123; });
    router_.get("/eh-non-std-throw",
                [](const HttpRequest::ptr &, HttpResponse &) {
                    throw std::runtime_error("boom");
                });

    HttpResponse response;
    bool found = router_.route(
        make_request(HttpMethod::GET, "/eh-non-std-throw"), response);
    EXPECT_TRUE(found);
    EXPECT_EQ(response.status_code(), HttpStatus::INTERNAL_SERVER_ERROR);
    EXPECT_EQ(response.body_content(), "Internal Server Error");
}

TEST_F(RouterTest, HomepageAliasTargetDoesNotRedirectAliasPath) {
    router_.set_homepage("/home");
    HttpResponse response;
    bool found = router_.route(make_request(HttpMethod::GET, "/"), response);
    EXPECT_FALSE(found);
    EXPECT_EQ(response.status_code(), HttpStatus::NOT_FOUND);
}

TEST_F(RouterTest, GroupAndPathUseIgnoreNullMiddlewareAndNormalizePrefix) {
    class TraceMiddleware final : public Middleware {
      public:
        explicit TraceMiddleware(std::vector<std::string> &trace)
            : trace_(trace) {}

        bool before(const HttpRequest::ptr &, HttpResponse &) override {
            trace_.push_back("before");
            return true;
        }

        void after(const HttpRequest::ptr &, HttpResponse &) override {
            trace_.push_back("after");
        }

      private:
        std::vector<std::string> &trace_;
    };

    std::vector<std::string> trace;
    router_.use(nullptr);
    router_.use("/api/users/", nullptr);
    router_.use_group("", std::make_shared<TraceMiddleware>(trace));
    router_.use_group("/", std::make_shared<TraceMiddleware>(trace));
    router_.use_group("/api///", std::make_shared<TraceMiddleware>(trace));
    router_.get("/api/users/",
                [&trace](const HttpRequest::ptr &, HttpResponse &) {
                    trace.push_back("handler");
                });

    HttpResponse response;
    bool found =
        router_.route(make_request(HttpMethod::GET, "/api/users/"), response);
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

        bool before(const HttpRequest::ptr &, HttpResponse &) override {
            before_called_ = true;
            return true;
        }

        void after(const HttpRequest::ptr &, HttpResponse &) override {
            after_called_ = true;
        }

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
        router_.route(make_request(HttpMethod::GET, "/not-found"), response);
    EXPECT_FALSE(found);
    EXPECT_TRUE(before_called);
    EXPECT_TRUE(after_called);
    EXPECT_EQ(response.status_code(), HttpStatus::NOT_FOUND);
}

int main(int argc, char **argv) {
    zhttp::init_logger();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
