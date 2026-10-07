#include "zhttp/internal/request_body.h"
#include "zhttp/internal/request_pipeline.h"

#include <gtest/gtest.h>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "zhttp/http_request.h"
#include "zhttp/http_response.h"
#include "zhttp/mid/middleware.h"
#include "zhttp/multipart.h"
#include "zhttp/router.h"
#include "zhttp/zhttp_logger.h"

namespace zhttp {
namespace {

TEST(RequestBodyTest, MultipartCacheUsesInjectedParserAndInvalidatesTogether) {
    detail::RequestBody body;
    body.set(std::string("payload"));
    int calls = 0;
    auto result = std::make_shared<MultipartFormData>();
    detail::RequestBody::MultipartParser parser = [&](std::string *) {
        ++calls;
        return result;
    };
    EXPECT_EQ(body.multipart("multipart/form-data", parser), result.get());
    EXPECT_TRUE(body.parse_multipart("multipart/form-data", parser));
    EXPECT_EQ(calls, 1);

    body.set(std::string("next payload"));
    EXPECT_EQ(body.multipart("multipart/form-data", parser), result.get());
    EXPECT_EQ(calls, 2);
    body.invalidate();
    detail::RequestBody::MultipartParser failed = [&](std::string *error) {
        ++calls;
        *error = "missing boundary";
        return std::shared_ptr<MultipartFormData>{};
    };
    EXPECT_FALSE(body.parse_multipart("multipart/form-data", failed));
    EXPECT_FALSE(body.parse_multipart("multipart/form-data", failed));
    EXPECT_EQ(body.multipart_error(), "missing boundary");
    EXPECT_EQ(calls, 3);
}

TEST(RequestCompatibilityTest, CopyAndMovePreserveParsedCacheOwnership) {
    static_assert(std::is_copy_constructible<HttpRequest>::value, "copy API");
    static_assert(std::is_copy_assignable<HttpRequest>::value, "copy API");
    static_assert(std::is_move_constructible<HttpRequest>::value, "move API");
    static_assert(std::is_move_assignable<HttpRequest>::value, "move API");
    HttpRequest original;
    original.set_header("Content-Type", "application/json");
    original.set_body("{\"id\":7}");
    ASSERT_TRUE(original.parse_json());
    const auto *parsed = original.json();

    HttpRequest copy = original;
    EXPECT_EQ(copy.json(), parsed);
    original.set_body("{\"id\":8}");
    EXPECT_EQ((*original.json())["id"], 8);
    EXPECT_EQ((*copy.json())["id"], 7);
    EXPECT_EQ(copy.body(), "{\"id\":7}");

    HttpRequest moved = std::move(copy);
    EXPECT_EQ(moved.json(), parsed);
    EXPECT_EQ((*moved.json())["id"], 7);
}

class TraceMiddleware : public mid::Middleware {
  public:
    TraceMiddleware(std::vector<std::string> &trace, std::string name)
        : trace_(trace), name_(std::move(name)) {}
    bool before(const HttpRequest::ptr &, HttpResponse &) override {
        trace_.push_back("before:" + name_);
        return true;
    }
    void after(const HttpRequest::ptr &, HttpResponse &) override {
        trace_.push_back("after:" + name_);
    }

  private:
    std::vector<std::string> &trace_;
    std::string name_;
};

TEST(RequestPipelineTest, GlobalGroupAndPathOrderDoesNotRequireRouter) {
    detail::RequestPipeline pipeline;
    std::vector<std::string> trace;
    pipeline.use(std::make_shared<TraceMiddleware>(trace, "global"));
    pipeline.use_group("/api/",
                       std::make_shared<TraceMiddleware>(trace, "api"));
    pipeline.use_group("/api/v1",
                       std::make_shared<TraceMiddleware>(trace, "v1"));
    pipeline.use("/api/v1/users",
                 std::make_shared<TraceMiddleware>(trace, "path"));
    auto request = std::make_shared<HttpRequest>();
    request->set_path("/api/v1/users");
    HttpResponse response;
    EXPECT_TRUE(pipeline.execute(
        request, response,
        [&](const HttpRequest::ptr &, HttpResponse &) {
            trace.push_back("handler");
        },
        true));
    EXPECT_EQ(trace, (std::vector<std::string>{
                         "before:global", "before:api", "before:v1",
                         "before:path", "handler", "after:path", "after:v1",
                         "after:api", "after:global"}));

    trace.clear();
    EXPECT_FALSE(pipeline.execute(request, response, {}, false));
    EXPECT_EQ(response.status_code(), HttpStatus::NOT_FOUND);
    EXPECT_EQ(trace, (std::vector<std::string>{"before:global", "before:path",
                                               "after:path", "after:global"}));
}

TEST(RequestPipelineTest, HandlerExceptionStillRunsCleanupInReverseOrder) {
    detail::RequestPipeline pipeline;
    std::vector<std::string> trace;
    pipeline.use(std::make_shared<TraceMiddleware>(trace, "global"));
    pipeline.set_exception_handler([&](const HttpRequest::ptr &,
                                       HttpResponse &response,
                                       std::exception_ptr exception) {
        trace.push_back("exception");
        EXPECT_THROW(std::rethrow_exception(exception), std::runtime_error);
        response.status(HttpStatus::INTERNAL_SERVER_ERROR);
    });
    HttpResponse response;
    EXPECT_TRUE(pipeline.execute(
        std::make_shared<HttpRequest>(), response,
        [](const HttpRequest::ptr &, HttpResponse &) {
            throw std::runtime_error("handler failed");
        },
        true));
    EXPECT_EQ(response.status_code(), HttpStatus::INTERNAL_SERVER_ERROR);
    EXPECT_EQ(trace, (std::vector<std::string>{"before:global", "exception",
                                               "after:global"}));
}

TEST(RouterCompatibilityTest,
     CopiedRouterKeepsRoutesAndMiddlewareRegistrations) {
    static_assert(std::is_copy_constructible<Router>::value, "copy API");
    static_assert(std::is_copy_assignable<Router>::value, "copy API");
    Router original;
    std::vector<std::string> trace;
    original.use(std::make_shared<TraceMiddleware>(trace, "global"));
    original.get("/users/:id",
                 [](const HttpRequest::ptr &request, HttpResponse &response) {
                     response.text(request->path_param("id"));
                 });
    Router copy = original;
    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    request->set_path("/users/7");
    HttpResponse response;
    EXPECT_TRUE(copy.route(request, response));
    EXPECT_EQ(response.body_content(), "7");
    EXPECT_EQ(trace,
              (std::vector<std::string>{"before:global", "after:global"}));
}

} // 命名空间
} // 命名空间 zhttp

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    zhttp::init_logger();
    return RUN_ALL_TESTS();
}
