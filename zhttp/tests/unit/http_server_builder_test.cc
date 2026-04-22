#include "zhttp/http_server_builder.h"
#include "zhttp/mid/middleware.h"
#include "zhttp/route_handler.h"
#include "zhttp/zhttp_logger.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace zhttp {
namespace {

class FixedHandler : public RouteHandler {
  public:
    explicit FixedHandler(std::string body) : body_(std::move(body)) {}

    void handle(const HttpRequest::ptr &, HttpResponse &response) override {
        response.status(HttpStatus::OK).text(body_);
    }

  private:
    std::string body_;
};

class NotFoundHandler : public RouteHandler {
  public:
    void handle(const HttpRequest::ptr &, HttpResponse &response) override {
        response.status(HttpStatus::NOT_FOUND).text("nf-handler");
    }
};

class MarkerMiddleware : public mid::Middleware {
  public:
    MarkerMiddleware(bool *before_called, bool *after_called)
        : before_called_(before_called), after_called_(after_called) {}

    bool before(const HttpRequest::ptr &, HttpResponse &response) override {
        if (before_called_) {
            *before_called_ = true;
        }
        response.header("X-MW-Before", "1");
        return true;
    }

    void after(const HttpRequest::ptr &, HttpResponse &response) override {
        if (after_called_) {
            *after_called_ = true;
        }
        response.header("X-MW-After", "1");
    }

  private:
    bool *before_called_;
    bool *after_called_;
};

uint16_t find_free_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return 0;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return 0;
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&addr), &len) != 0) {
        ::close(fd);
        return 0;
    }

    const uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

HttpRequest::ptr make_request(HttpMethod method, const std::string &path) {
    auto request = std::make_shared<HttpRequest>();
    request->set_method(method);
    request->set_version(HttpVersion::HTTP_1_1);
    request->set_path(path);
    return request;
}

TEST(HttpServerBuilderTest, BuildConfiguresRoutesMiddlewareAndHandlers) {
    const uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    bool before_called = false;
    bool after_called = false;

    HttpServerBuilder builder;
    builder.listen("127.0.0.1", port)
        .threads(1)
        .stack_mode(StackMode::SHARED)
        .use_shared_stack()
        .use_independent_stack()
        .read_timeout(123)
        .write_timeout(456)
        .keepalive_timeout(789)
        .log_level("WARN")
        .log_async(false)
        .log_format("[%p] %m")
        .log_sink("file+stdout")
        .log_file("/tmp/zhttp-builder.log")
        .daemon(false)
        .homepage("landing")
        .server_name("builder-test-server");

    builder.use(nullptr);
    builder.use(
        std::make_shared<MarkerMiddleware>(&before_called, &after_called));

    builder.get("/cb-get", [](const HttpRequest::ptr &, HttpResponse &resp) {
        resp.status(HttpStatus::OK).text("cb-get");
    });
    builder.post("/cb-post", [](const HttpRequest::ptr &, HttpResponse &resp) {
        resp.status(HttpStatus::OK).text("cb-post");
    });
    builder.put("/cb-put", [](const HttpRequest::ptr &, HttpResponse &resp) {
        resp.status(HttpStatus::OK).text("cb-put");
    });
    builder.del("/cb-del", [](const HttpRequest::ptr &, HttpResponse &resp) {
        resp.status(HttpStatus::OK).text("cb-del");
    });

    builder.get("/h-get", std::make_shared<FixedHandler>("h-get"));
    builder.post("/h-post", std::make_shared<FixedHandler>("h-post"));
    builder.put("/h-put", std::make_shared<FixedHandler>("h-put"));
    builder.del("/h-del", std::make_shared<FixedHandler>("h-del"));

    builder.not_found(
        [](const HttpRequest::ptr &, HttpResponse &resp) { resp.text("tmp"); });
    builder.not_found(std::make_shared<NotFoundHandler>());
    builder.exception_handler(
        [](const HttpRequest::ptr &, HttpResponse &resp, std::exception_ptr) {
            resp.status(HttpStatus::INTERNAL_SERVER_ERROR).text("custom-ex");
        });
    builder.get("/throw", [](const HttpRequest::ptr &, HttpResponse &) {
        throw std::runtime_error("boom");
    });

    auto server = builder.build();
    ASSERT_NE(server, nullptr);

    {
        auto request = make_request(HttpMethod::GET, "/");
        HttpResponse response;
        EXPECT_TRUE(server->router().route(request, response));
        EXPECT_EQ(response.status_code(), HttpStatus::FOUND);
        EXPECT_EQ(response.headers().at("Location"), "/landing");
    }

    struct Case {
        HttpMethod method;
        const char *path;
        const char *body;
    };
    const Case cases[] = {
        {HttpMethod::GET, "/cb-get", "cb-get"},
        {HttpMethod::POST, "/cb-post", "cb-post"},
        {HttpMethod::PUT, "/cb-put", "cb-put"},
        {HttpMethod::DELETE, "/cb-del", "cb-del"},
        {HttpMethod::GET, "/h-get", "h-get"},
        {HttpMethod::POST, "/h-post", "h-post"},
        {HttpMethod::PUT, "/h-put", "h-put"},
        {HttpMethod::DELETE, "/h-del", "h-del"},
    };

    for (const auto &item : cases) {
        auto request = make_request(item.method, item.path);
        HttpResponse response;
        EXPECT_TRUE(server->router().route(request, response));
        EXPECT_EQ(response.status_code(), HttpStatus::OK);
        EXPECT_EQ(response.body_content(), item.body);
        EXPECT_EQ(response.headers().at("X-MW-Before"), "1");
        EXPECT_EQ(response.headers().at("X-MW-After"), "1");
    }

    {
        auto request = make_request(HttpMethod::GET, "/throw");
        HttpResponse response;
        EXPECT_TRUE(server->router().route(request, response));
        EXPECT_EQ(response.status_code(), HttpStatus::INTERNAL_SERVER_ERROR);
        EXPECT_EQ(response.body_content(), "custom-ex");
    }

    {
        auto request = make_request(HttpMethod::GET, "/missing");
        HttpResponse response;
        EXPECT_FALSE(server->router().route(request, response));
        EXPECT_EQ(response.status_code(), HttpStatus::NOT_FOUND);
        EXPECT_EQ(response.body_content(), "nf-handler");
    }

    EXPECT_TRUE(before_called);
    EXPECT_TRUE(after_called);
}

TEST(HttpServerBuilderTest, BuildThrowsForInvalidConfigAndLookupFailure) {
    HttpServerBuilder invalid_config_builder;
    invalid_config_builder.listen("127.0.0.1", 0).threads(1).log_level("error");
    EXPECT_THROW(invalid_config_builder.build(), std::runtime_error);

    HttpServerBuilder resolve_fail_builder;
    resolve_fail_builder.listen("invalid host name", 18080)
        .threads(1)
        .log_level("error");
    EXPECT_THROW(resolve_fail_builder.build(), std::runtime_error);
}

TEST(HttpServerBuilderTest, BuildThrowsWhenSslInitializationFails) {
    const uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    HttpServerBuilder builder;
    builder.listen("127.0.0.1", port)
        .threads(1)
        .log_level("error")
        .enable_https("/tmp/not-exist-cert.pem", "/tmp/not-exist-key.pem");

    EXPECT_THROW(builder.build(), std::runtime_error);
}

TEST(HttpServerBuilderTest, RunStopsGracefullyAfterSignalInForegroundMode) {
    const uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    HttpServerBuilder builder;
    builder.listen("127.0.0.1", port)
        .threads(1)
        .daemon(false)
        .log_level("error")
        .get("/ok", [](const HttpRequest::ptr &, HttpResponse &resp) {
            resp.status(HttpStatus::OK).text("ok");
        });

    std::thread stopper([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::raise(SIGINT);
    });

    EXPECT_NO_THROW(builder.run());
    stopper.join();
}

TEST(HttpServerBuilderTest, RunThrowsWhenBuildFails) {
    HttpServerBuilder builder;
    builder.listen("127.0.0.1", 0).threads(1).daemon(false).log_level("error");
    EXPECT_THROW(builder.run(), std::runtime_error);
}

} // namespace
} // namespace zhttp

int main(int argc, char **argv) {
    zhttp::init_logger();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
