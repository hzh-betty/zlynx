#include "../support/network_fixture.h"
#include "../support/server_fixture.h"
#include "../support/signal_server.h"
#include "zhttp/http_server.h"
#include "zhttp/http_server_builder.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace zhttp;
using namespace zhttp::mid;
using namespace zhttp::test;

namespace {

constexpr const char *kTestCertPem =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDCTCCAfGgAwIBAgIUMZG0HpTQPHQ+PHcqr2qKSeOUbAgwDQYJKoZIhvcNAQEL\n"
    "BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDQwMjA3NDcwNVoXDTI2MDQw\n"
    "MzA3NDcwNVowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF\n"
    "AAOCAQ8AMIIBCgKCAQEAnIGUGwBo8HC7dtFiS+fTopQyO97ZVtyqKqK6irNWo88l\n"
    "hEQdxIo8Xdw6MX86+sdJbSkJKu93BfnnVoCWg2SK/r5IJlsUxLbLViX32PyC+UP3\n"
    "RmJff1FRkCf8Cw6Z0iajkIbBWPrPS9uAmDiczk/NS4f3DARjl3K6oj4xzr/mxi6k\n"
    "W2w2qGfreFrpVTCDPOcdxoRc65b3Uxzr9OtM49a8CGLUsEyG9PapVdIGmVWjAuS0\n"
    "ELNOVojGAvOuh8w5EZBgWR4uAwfWVYUXJ14HB5Vd/JLJS+lPlUBmeLGsiIkrG4Aj\n"
    "eTi77WxiRBKNeuW6gSwyfCE126xDhlU90TZjqfswqQIDAQABo1MwUTAdBgNVHQ4E\n"
    "FgQUc5z1DTJecX7iiYr3ptmOD0QIwSQwHwYDVR0jBBgwFoAUc5z1DTJecX7iiYr3\n"
    "ptmOD0QIwSQwDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOCAQEAUpu/\n"
    "wwFhoTO7IlpNy6iQ6gW113Bh86xV2HICoJ1SsRY+GdSN2dWB8xWFknPDhwMH/QDM\n"
    "8iudFx/WlTt9TZeh5AVZcUmX4tvdBVWFtxQNkMV8iUy2J+t6VnTFlipkqXdO5FOG\n"
    "AvbixjLaYzZycd8V9TaCLaz3oi2/2BaOCoFYOPKW4KdzaKap0JbLB/qbPAcZh5mb\n"
    "FC35W42+2Q9GwIwF7IwX8n2IJeqhl8kxYLILIU98PtaCpjkM5j7w6QTi4uxpYOav\n"
    "727tmMqPDLbLsMzbe/KvhcX/5tSRzuEDvalmHCUfCKN0AwYp1UJdpS3Iy88tIG3z\n"
    "5N6lMvR5hwbSLNJ/FA==\n"
    "-----END CERTIFICATE-----\n";

constexpr const char *kTestKeyPem =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQCcgZQbAGjwcLt2\n"
    "0WJL59OilDI73tlW3KoqorqKs1ajzyWERB3Eijxd3Doxfzr6x0ltKQkq73cF+edW\n"
    "gJaDZIr+vkgmWxTEtstWJffY/IL5Q/dGYl9/UVGQJ/wLDpnSJqOQhsFY+s9L24CY\n"
    "OJzOT81Lh/cMBGOXcrqiPjHOv+bGLqRbbDaoZ+t4WulVMIM85x3GhFzrlvdTHOv0\n"
    "60zj1rwIYtSwTIb09qlV0gaZVaMC5LQQs05WiMYC866HzDkRkGBZHi4DB9ZVhRcn\n"
    "XgcHlV38kslL6U+VQGZ4sayIiSsbgCN5OLvtbGJEEo165bqBLDJ8ITXbrEOGVT3R\n"
    "NmOp+zCpAgMBAAECggEADvgz7ts8rlbSD3zajpEXhJLxNRnNJwpHOnnDJvYdYOC0\n"
    "4XBUepKQUJf6dvaI8SizpL3KkzFBbLBpCLSW8O1igBg6NXT7yQ8E5vINwVLxwh7W\n"
    "nYRWRwrDpuq0KGiWnOu2NGI3RygCQOq3Y5fyo6ctZz6TijI3RmqiYFdbkz92ttjj\n"
    "2mcBCz+nVBa5bUKm087jLsFrGde9ZVK0gClDKM0VoRTM5RTGtXirqueEwyw9Dg/r\n"
    "y0nWz5YwuuZ6tDTEn/cnxZ5sAPOmgrO6zn5Y67Q93DJa5QQTM0SyfgGnxdwLmiQ6\n"
    "Xb4uWTPkPG+k4DTWiigKPzMPINEEt6nI0soFjM0v2wKBgQDXLaJKQuKRh2N8TzeO\n"
    "OeW6i7b/X+dvFZ6iXZid67P3eiN2hoYI+nYqmDzUYQ3L8/iSJ1HEpL9G3U+TakGT\n"
    "Q2/qEEUFbryOb7XH7mr70vSDx3Ac0nOFX/fklRoOemlpY7n4iOjLuSS0La1A9t58\n"
    "5IwhBHVSca4xtdLl1b6MxxU7twKBgQC6Mnh+DzMJdTjJyj34utBCL1z2K9KFhZc0\n"
    "3uc5AL4vE+V7XjSvQt+vdWRdvxxl+6B73yU25W2YgSHtTod9UvcKnxey2GMSOH/L\n"
    "m/pbg3tmeNaBStfwF8K/pl5PB+dMVq+fzRDwsvQA0Gw6krOCq8X4i7s6/6QzpGxa\n"
    "NHxhDWG2nwKBgEhZBI75bBpILi/2ppRAbThKj43Png3gdATdeVnnjQvxWgkY8+oC\n"
    "5EYwB4vU0gG4FuR1Ke33AoT+FipXeJLeArvtGnfYIre1YaZGSFxBMos4PD7El6jJ\n"
    "epy1cRxbFiQkLrwctEEDEA8wqGcGWgoeAet8B0JgDJSUMMOsGTRWH5KDAoGAGU+1\n"
    "G4XbbUS3JI9On1pd5zFjFL/eTXJcnL5UdmZIdEPjJUMoLE8N818k9q19Icv0BALQ\n"
    "n0bPADVFtGnBd2Lo3FPGN/S8ewSdMsOQZBJamxKALnFLK4M/YSgvl9S+N51tIG8T\n"
    "B3V8QAQVQl0g8/l/3wq3uAx6eN64MAcEhXj5OIcCgYAVxcANjy4nPEZRM5sg1BLR\n"
    "xN0l97sqB8I+KbgTXAe/nKDFVuYNes7/j/JatBX2GrQqZO8kGK0srNm/hQ7GWH1R\n"
    "MummkzUztkUyvZP65kH+n9ijgfUWu9sYAeUikex9e3l6cBOZMx1e3S5SgVm+dulp\n"
    "80tKGyIUG90rg8uO6f3Uww==\n"
    "-----END PRIVATE KEY-----\n";

std::string write_temp_pem_file(const char *template_path,
                                const char *pem_content) {
    std::vector<char> buffer(template_path,
                             template_path + std::strlen(template_path) + 1);
    const int fd = ::mkstemps(buffer.data(), 4);
    if (fd < 0) {
        return "";
    }

    zco::io::Descriptor file(fd);
    const std::string path(buffer.data());
    std::ofstream output(path, std::ios::binary);
    output << pem_content;
    output.close();
    if (!output.good()) {
        std::remove(path.c_str());
        return "";
    }
    return path;
}

class ScopedTlsPemFiles {
  public:
    ScopedTlsPemFiles()
        : cert_path_(write_temp_pem_file("/tmp/zhttp-test-cert-XXXXXX.pem",
                                         kTestCertPem)),
          key_path_(write_temp_pem_file("/tmp/zhttp-test-key-XXXXXX.pem",
                                        kTestKeyPem)) {}

    ~ScopedTlsPemFiles() {
        if (!cert_path_.empty()) {
            std::remove(cert_path_.c_str());
        }
        if (!key_path_.empty()) {
            std::remove(key_path_.c_str());
        }
    }

    const std::string &cert_path() const { return cert_path_; }
    const std::string &key_path() const { return key_path_; }

  private:
    std::string cert_path_;
    std::string key_path_;
};

template <class Operation>
int tls_call(SSL *ssl, zco::Deadline deadline, Operation operation) {
    for (;;) {
        if (deadline.expired(zco::Deadline::Clock::now()))
            throw std::system_error(std::make_error_code(std::errc::timed_out));
        const int result = operation();
        if (result > 0)
            return result;
        const int error = SSL_get_error(ssl, result);
        if (error == SSL_ERROR_ZERO_RETURN)
            return 0;
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
            throw std::runtime_error("TLS client error " +
                                     std::to_string(error));
        if (auto failed = wait_ready(
                SSL_get_fd(ssl),
                error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT, deadline))
            throw std::system_error(failed);
    }
}

} // namespace

// 注意：集成测试需要网络环境，可能需要特殊配置
// 这里提供基本的路由和中间件测试

TEST(HttpServerIntegrationTest, RouteRegistration) {
    bool handler_called = false;
    HttpApplication router;

    router.router().get("/test", [&handler_called](HttpContext &context) {
        auto &resp = context.response();
        handler_called = true;
        resp.status(HttpStatus::OK).text("OK");
    });

    // 模拟路由匹配
    auto request = std::make_shared<TestContext>();
    request->set_method(HttpMethod::GET);
    request->set_path("/test");
    HttpResponse response;

    bool found = run_application(router, request, response);

    EXPECT_TRUE(found);
    EXPECT_TRUE(handler_called);
    EXPECT_EQ(response.status_code(), HttpStatus::OK);
}

TEST(HttpServerIntegrationTest, MiddlewareIntegration) {
    // 创建一个简单的中间件
    class TestMiddleware : public Middleware {
      public:
        TestMiddleware(bool &before_called, bool &after_called)
            : before_called_(before_called), after_called_(after_called) {}

        bool before(HttpContext &context) override {
            auto &resp = context.response();
            before_called_ = true;
            resp.header("X-Test-Middleware", "before");
            return true;
        }

        void after(HttpContext &context) override {
            auto &resp = context.response();
            after_called_ = true;
            resp.header("X-Test-After", "after");
        }

      private:
        bool &before_called_;
        bool &after_called_;
    };

    bool before_called = false;
    bool after_called = false;
    HttpApplication router;

    router.use(std::make_shared<TestMiddleware>(before_called, after_called));
    router.router().get("/middleware-test", [](HttpContext &context) {
        auto &resp = context.response();
        resp.status(HttpStatus::OK).text("OK");
    });

    auto request = std::make_shared<TestContext>();
    request->set_method(HttpMethod::GET);
    request->set_path("/middleware-test");
    HttpResponse response;

    run_application(router, request, response);

    EXPECT_TRUE(before_called);
    EXPECT_TRUE(after_called);
    EXPECT_EQ(response.headers().at("X-Test-Middleware"), "before");
    EXPECT_EQ(response.headers().at("X-Test-After"), "after");
}

TEST(HttpServerIntegrationTest, NotFoundRoute) {
    HttpApplication router;
    auto request = std::make_shared<TestContext>();
    request->set_method(HttpMethod::GET);
    request->set_path("/nonexistent");
    HttpResponse response;

    bool found = run_application(router, request, response);

    EXPECT_FALSE(found);
    EXPECT_EQ(response.status_code(), HttpStatus::NOT_FOUND);
}

TEST(HttpServerIntegrationTest, KeepsParserStateAcrossSplitPackets) {

    auto server = make_server();
    server->router().get("/split", [](HttpContext &context) {
        auto &resp = context.response();
        resp.status(HttpStatus::OK).text("split-ok");
    });

    ASSERT_TRUE(server);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());

    auto client = connect_to(*server);
    ASSERT_TRUE(client) << client.error().message();
    const int client_fd = client.value().native_handle();

    const std::string first_chunk = "GET /split HTTP/1.1\r\n"
                                    "Host: localhost\r\n"
                                    "Connection: close";
    const std::string second_chunk = "\r\n\r\n";

    ASSERT_TRUE(send_all(client_fd, first_chunk));
    pollfd partial{client_fd, POLLIN, 0};
    EXPECT_EQ(::poll(&partial, 1, 50), 0) << "未完整的请求不应产生响应";
    ASSERT_TRUE(send_all(client_fd, second_chunk));

    const std::string response = recv_until_close(client_fd, 1000);

    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos) << response;
    EXPECT_NE(response.find("split-ok"), std::string::npos) << response;
}

TEST(HttpServerIntegrationTest, HandlesChunkedRequestBodyEndToEnd) {

    auto server = make_server();
    server->router().post("/upload", [](HttpContext &context) {
        auto *req = &context;
        auto &resp = context.response();
        resp.status(HttpStatus::OK).text(req->body());
    });

    ASSERT_TRUE(server);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());

    auto client = connect_to(*server);
    ASSERT_TRUE(client) << client.error().message();
    const int client_fd = client.value().native_handle();

    // 请求体由两个 chunk 组成：4=Wiki、5=pedia，最后用 0 块结束。
    const std::string request = "POST /upload HTTP/1.1\r\n"
                                "Host: localhost\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "Connection: close\r\n"
                                "\r\n"
                                "4\r\n"
                                "Wiki\r\n"
                                "5\r\n"
                                "pedia\r\n"
                                "0\r\n"
                                "\r\n";

    ASSERT_TRUE(send_all(client_fd, request));
    const std::string response = recv_until_close(client_fd, 1000);

    // 服务端会先把 chunk 合并成完整 body，再由业务逻辑回显。
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos) << response;
    EXPECT_NE(response.find("\r\n\r\nWikipedia"), std::string::npos)
        << response;
}

TEST(HttpServerIntegrationTest, SendsExplicitChunkedResponseBody) {

    auto server = make_server();
    server->router().get(
        "/chunked-static", [](HttpContext &context) {
            auto &resp = context.response();
            resp.status(HttpStatus::OK)
                .content_type("text/plain")
                .body("hello")
                .enable_chunked();
        });

    ASSERT_TRUE(server);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());

    auto client = connect_to(*server);
    ASSERT_TRUE(client) << client.error().message();
    const int client_fd = client.value().native_handle();

    const std::string request = "GET /chunked-static HTTP/1.1\r\n"
                                "Host: localhost\r\n"
                                "Connection: close\r\n"
                                "\r\n";

    ASSERT_TRUE(send_all(client_fd, request));
    const std::string response = recv_until_close(client_fd, 1000);

    // 显式 enable_chunked 后，body 应按 chunk 帧输出且不再附带 Content-Length。
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos) << response;
    EXPECT_NE(response.find("Transfer-Encoding: chunked"), std::string::npos)
        << response;
    EXPECT_EQ(response.find("Content-Length:"), std::string::npos) << response;
    EXPECT_NE(response.find("\r\n\r\n5\r\nhello\r\n0\r\n\r\n"),
              std::string::npos)
        << response;
}

TEST(HttpServerIntegrationTest, SendsChunkedStreamResponse) {

    auto server = make_server();
    server->router().get(
        "/chunked-stream", [](HttpContext &context) {
            auto &resp = context.response();
            resp.status(HttpStatus::OK)
                .content_type("text/plain")
                .stream([chunks = std::vector<std::string>{"Wiki", "pedia"},
                         index = size_t{0}](char *buffer,
                                            size_t size) mutable -> size_t {
                    // 每次回调返回一段数据，服务端会把该段编码为独立 chunk。
                    if (index >= chunks.size()) {
                        return 0;
                    }

                    const std::string &chunk = chunks[index++];
                    if (chunk.size() > size) {
                        return 0;
                    }

                    std::memcpy(buffer, chunk.data(), chunk.size());
                    return chunk.size();
                });
        });

    ASSERT_TRUE(server);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());

    auto client = connect_to(*server);
    ASSERT_TRUE(client) << client.error().message();
    const int client_fd = client.value().native_handle();

    const std::string request = "GET /chunked-stream HTTP/1.1\r\n"
                                "Host: localhost\r\n"
                                "Connection: close\r\n"
                                "\r\n";

    ASSERT_TRUE(send_all(client_fd, request));
    const std::string response = recv_until_close(client_fd, 1000);

    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos) << response;
    EXPECT_NE(response.find("Transfer-Encoding: chunked"), std::string::npos)
        << response;
    EXPECT_EQ(response.find("Content-Length:"), std::string::npos) << response;
    EXPECT_NE(response.find("\r\n\r\n4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n"),
              std::string::npos)
        << response;
}

znet::Socket bind_and_listen_port() {
    auto listener =
        znet::Socket::create(AF_INET, znet::SocketKind::stream).value();
    listener.bind(znet::Endpoint::ipv4("127.0.0.1", 0).value()).value();
    listener.listen().value();
    return listener;
}

TEST(HttpServerIntegrationTest, ReturnsBadRequestForMalformedHttpRequest) {

    auto server = make_server();
    server->router().get("/ok", [](HttpContext &context) {
        auto &resp = context.response();
        resp.status(HttpStatus::OK).text("ok");
    });

    ASSERT_TRUE(server);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());

    auto client = connect_to(*server);
    ASSERT_TRUE(client) << client.error().message();
    const int client_fd = client.value().native_handle();

    // Host 头缺少 ':'，应触发解析错误并返回 400。
    const std::string malformed = "GET /ok HTTP/1.1\r\n"
                                  "Host localhost\r\n"
                                  "Connection: close\r\n"
                                  "\r\n";

    ASSERT_TRUE(send_all(client_fd, malformed));
    const std::string response = recv_until_close(client_fd, 1000);

    EXPECT_NE(response.find("400 Bad Request"), std::string::npos) << response;
    EXPECT_NE(response.find("Bad Request"), std::string::npos) << response;
}

TEST(HttpServerIntegrationTest, DoesNotUseChunkedForNoContentStatus) {

    auto server = make_server();
    server->router().get("/no-content", [](HttpContext &context) {
        auto &resp = context.response();
        resp.status(HttpStatus::NO_CONTENT).body("ignored").enable_chunked();
    });

    ASSERT_TRUE(server);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());

    auto client = connect_to(*server);
    ASSERT_TRUE(client) << client.error().message();
    const int client_fd = client.value().native_handle();

    const std::string request = "GET /no-content HTTP/1.1\r\n"
                                "Host: localhost\r\n"
                                "Connection: close\r\n"
                                "\r\n";
    ASSERT_TRUE(send_all(client_fd, request));
    const std::string response = recv_until_close(client_fd, 1000);

    EXPECT_NE(response.find("204 No Content"), std::string::npos) << response;
    EXPECT_EQ(response.find("Transfer-Encoding: chunked"), std::string::npos)
        << response;
}

TEST(HttpServerIntegrationTest, HttpsRoundTripWithRealTlsHandshake) {

    ScopedTlsPemFiles pem_files;
    ASSERT_FALSE(pem_files.cert_path().empty());
    ASSERT_FALSE(pem_files.key_path().empty());

    auto server = make_server();
    ASSERT_TRUE(server->set_ssl_certificate(pem_files.cert_path(),
                                            pem_files.key_path()));
    server->router().get("/secure", [](HttpContext &context) {
        auto &resp = context.response();
        resp.status(HttpStatus::OK).text("secure-ok");
    });

    ASSERT_TRUE(server);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());

    auto client = connect_to(*server);
    ASSERT_TRUE(client) << client.error().message();
    const int client_fd = client.value().native_handle();

    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> ssl_ctx(
        SSL_CTX_new(TLS_client_method()), SSL_CTX_free);
    ASSERT_NE(ssl_ctx, nullptr);
    SSL_CTX_set_verify(ssl_ctx.get(), SSL_VERIFY_NONE, nullptr);

    std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(ssl_ctx.get()),
                                                  SSL_free);
    ASSERT_NE(ssl, nullptr);
    ASSERT_EQ(SSL_set_fd(ssl.get(), client_fd), 1);
    const auto deadline = zco::Deadline::after(std::chrono::seconds(2));
    ASSERT_EQ(
        tls_call(ssl.get(), deadline, [&] { return SSL_connect(ssl.get()); }),
        1);

    const std::string request = "GET /secure HTTP/1.1\r\n"
                                "Host: localhost\r\n"
                                "Connection: close\r\n\r\n";

    size_t sent = 0;
    while (sent < request.size()) {
        const int n = tls_call(ssl.get(), deadline, [&] {
            return SSL_write(ssl.get(), request.data() + sent,
                             static_cast<int>(request.size() - sent));
        });
        ASSERT_GT(n, 0);
        sent += static_cast<size_t>(n);
    }

    std::string response;
    char read_buffer[1024];
    while (true) {
        const int n = tls_call(ssl.get(), deadline, [&] {
            return SSL_read(ssl.get(), read_buffer, sizeof(read_buffer));
        });
        if (!n)
            break;
        response.append(read_buffer, static_cast<size_t>(n));
    }
    SSL_shutdown(ssl.get());

    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos) << response;
    EXPECT_NE(response.find("secure-ok"), std::string::npos) << response;
}

TEST(HttpServerIntegrationTest, RunServesHttpAndStopsOnSignal) {
    auto server = start_signal_server();
    EXPECT_NE(server.response.find("200 OK"), std::string::npos)
        << server.response;
    EXPECT_EQ(server.response.find("Location:"), std::string::npos);
    ASSERT_EQ(::kill(server.child->pid(), SIGINT), 0);
    ASSERT_TRUE(
        server.child->wait(zco::Deadline::after(std::chrono::seconds(2))));
    ASSERT_TRUE(WIFEXITED(server.child->status()));
    EXPECT_EQ(WEXITSTATUS(server.child->status()), 0);
}

TEST(HttpServerIntegrationTest,
     RunFailsWhenHttpsPortAlreadyInUse) {
    auto occupied_https = bind_and_listen_port();
    const uint16_t https_port = occupied_https.local_endpoint().value().port();
    ASSERT_NE(https_port, 0);

    ScopedTlsPemFiles pem_files;
    ASSERT_FALSE(pem_files.cert_path().empty());
    ASSERT_FALSE(pem_files.key_path().empty());

    HttpServerBuilder builder;
    builder.listen("127.0.0.1", https_port)
        .threads(1)
        .daemon(false)
        .log_level("error")
        .enable_https(pem_files.cert_path(), pem_files.key_path());

    EXPECT_THROW(builder.run(), std::runtime_error);
}

TEST(HttpServerIntegrationTest, HandlesStreamErrorPaths) {

    auto server = make_server();
    server->router()
        .get("/chunked-stream-throw",
             [](HttpContext &context) {
                 auto &resp = context.response();
                 resp.status(HttpStatus::OK)
                     .content_type("text/plain")
                     .stream([](char *, size_t) -> size_t {
                         throw std::runtime_error("stream boom");
                     });
             });
    server->router().get(
        "/chunked-stream-oversize", [](HttpContext &context) {
            auto &resp = context.response();
            resp.status(HttpStatus::OK)
                .content_type("text/plain")
                .stream([](char *, size_t size) -> size_t { return size + 1; });
        });

    ASSERT_TRUE(server);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());

    {
        auto client = connect_to(*server);
        ASSERT_TRUE(client) << client.error().message();
        const int client_fd = client.value().native_handle();
        const std::string request = "GET /chunked-stream-throw HTTP/1.1\r\n"
                                    "Host: localhost\r\n"
                                    "Connection: close\r\n\r\n";
        ASSERT_TRUE(send_all(client_fd, request));
        const std::string response = recv_until_close(client_fd, 1000);
        EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos)
            << response;
        EXPECT_NE(response.find("Transfer-Encoding: chunked"),
                  std::string::npos)
            << response;
        EXPECT_EQ(response.find("0\r\n\r\n"), std::string::npos) << response;
    }

    {
        auto client = connect_to(*server);
        ASSERT_TRUE(client) << client.error().message();
        const int client_fd = client.value().native_handle();
        const std::string request = "GET /chunked-stream-oversize HTTP/1.1\r\n"
                                    "Host: localhost\r\n"
                                    "Connection: close\r\n\r\n";
        ASSERT_TRUE(send_all(client_fd, request));
        const std::string response = recv_until_close(client_fd, 1000);
        EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos)
            << response;
        EXPECT_NE(response.find("Transfer-Encoding: chunked"),
                  std::string::npos)
            << response;
        EXPECT_EQ(response.find("0\r\n\r\n"), std::string::npos) << response;
    }
}

TEST(HttpServerIntegrationTest, RejectsOversizedBodyBeforeBodyIsSent) {
    auto server = make_server(2);
    ScopedServer cleanup(server);
    RequestLimits limits;
    limits.max_body_bytes = 4;
    server->set_request_limits(limits);
    ASSERT_TRUE(server->start());
    auto client = connect_to(*server);
    ASSERT_TRUE(client) << client.error().message();
    const int fd = client.value().native_handle();
    EXPECT_TRUE(send_all(fd, "POST / HTTP/1.1\r\nContent-Length: 5\r\n\r\n"));
    const std::string response = recv_until_close(fd, 1000);
    EXPECT_NE(response.find("413"), std::string::npos);
}

TEST(HttpServerIntegrationTest,
     RequestDeadlineClosesSilentPartialRequestWithInfiniteReadTimeout) {
    auto server = make_server(2);
    ScopedServer cleanup(server);
    server->set_request_timeout(50);
    server->set_recv_timeout(0);
    ASSERT_TRUE(server->start());
    auto client = connect_to(*server);
    ASSERT_TRUE(client) << client.error().message();
    const int fd = client.value().native_handle();
    EXPECT_TRUE(send_all(fd, "GET / HTTP/1.1\r\nX: "));
    pollfd pfd{fd, POLLIN | POLLHUP, 0};
    const int ready = ::poll(&pfd, 1, 2000);
    EXPECT_GT(ready, 0);
    if (ready > 0) {
        char byte;
        EXPECT_EQ(::recv(fd, &byte, 1, MSG_DONTWAIT), 0);
    }
}

TEST(HttpServerIntegrationTest, SlowFragmentsDoNotRenewRequestDeadline) {
    auto server = make_server(2);
    ScopedServer cleanup(server);
    server->set_request_timeout(80);
    server->set_recv_timeout(0);
    ASSERT_TRUE(server->start());
    auto client = connect_to(*server);
    ASSERT_TRUE(client) << client.error().message();
    const int fd = client.value().native_handle();
    EXPECT_TRUE(send_all(fd, "GET / HTTP/1.1\r\nX: "));
    bool closed = false;
    for (int i = 0; i < 50 && !closed; ++i) {
        ::send(fd, "a", 1, MSG_DONTWAIT | MSG_NOSIGNAL);
        pollfd pfd{fd, POLLIN | POLLHUP, 0};
        if (::poll(&pfd, 1, 10) > 0) {
            char byte;
            closed = ::recv(fd, &byte, 1, MSG_DONTWAIT) == 0;
        }
    }
    EXPECT_TRUE(closed);
}

TEST(HttpServerIntegrationTest, CompleteRequestClearsDeadlineForKeepAlive) {
    auto server = make_server(2);
    ScopedServer cleanup(server);
    server->set_request_timeout(30);
    server->set_recv_timeout(0);
    server->router().get("/", [](HttpContext &context) {
        auto &response = context.response();
        response.text("ok");
    });
    ASSERT_TRUE(server->start());
    auto client = connect_to(*server);
    ASSERT_TRUE(client) << client.error().message();
    const int fd = client.value().native_handle();
    EXPECT_TRUE(send_all(fd, "GET / HTTP/1.1\r\n\r\n"));
    SocketReader reader(fd, 1);
    const auto deadline = zco::Deadline::after(std::chrono::seconds(1));
    const auto headers = reader.until("\r\n\r\n", deadline).value();
    ASSERT_NE(headers.find("Content-Length: 2\r\n"), std::string::npos)
        << headers;
    EXPECT_EQ(reader.exact(2, deadline).value(), "ok");
    pollfd idle{fd, POLLIN, 0};
    EXPECT_EQ(::poll(&idle, 1, 100), 0) << "完整请求后的空闲连接应继续保持";
    const std::string request = "GET / HTTP/1.1\r\nConnection: close\r\n\r\n";
    ASSERT_TRUE(send_all(fd, request));
    const std::string response = recv_until_close(fd, 1000);
    EXPECT_NE(response.find("200 OK"), std::string::npos);
}

TEST(HttpServerIntegrationTest,
     SynchronousStreamProcessesPipelinedRequestWithoutNewInput) {
    std::atomic<int> completed{0};
    auto server = make_server();
    server->router().get("/stream", [&](HttpContext &context) {
        context.on_complete([&](CompletionResult result) {
            EXPECT_EQ(result, CompletionResult::Completed);
            ++completed;
        });
        context.response().stream(
            [remaining = 1](char *buffer, size_t) mutable {
                if (!remaining--)
                    return size_t{0};
                std::memcpy(buffer, "data", 4);
                return size_t{4};
            });
    });
    server->router().get("/next", [&](HttpContext &context) {
        EXPECT_EQ(completed.load(), 1);
        context.response().text("next");
    });
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());
    auto client = connect_to(*server);
    ASSERT_TRUE(client) << client.error().message();
    const int fd = client.value().native_handle();
    ASSERT_TRUE(send_all(fd,
        "GET /stream HTTP/1.1\r\nHost: test\r\n\r\n"
        "GET /next HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n"));
    const auto response = recv_until_close(fd, 1000);
    EXPECT_NE(response.find("4\r\ndata\r\n0\r\n\r\nHTTP/1.1 200 OK"),
              std::string::npos)
        << response;
    EXPECT_EQ(response.substr(response.size() - 4), "next");
    EXPECT_EQ(completed.load(), 1);
}

TEST(HttpServerIntegrationTest, LargePullStreamIsFullyDrained) {
    const size_t blocks = 160, block_size = 8192;
    auto server = make_server();
    server->router().get(
        "/large", [=](HttpContext &context) {
            context.response().stream(
                [remaining = blocks](char *buffer,
                                     size_t size) mutable -> size_t {
                    if (!remaining)
                        return 0;
                    --remaining;
                    std::memset(buffer, 'x', size);
                    return size;
                });
        });
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());
    auto client = connect_to(*server);
    ASSERT_TRUE(client) << client.error().message();
    const int fd = client.value().native_handle();
    ASSERT_TRUE(send_all(
        fd, "GET /large HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n"));
    const auto wire = recv_until_close(fd, 2000);
    const auto start = wire.find("\r\n\r\n");
    ASSERT_NE(start, std::string::npos);
    const std::string chunk =
        "2000\r\n" + std::string(block_size, 'x') + "\r\n";
    size_t offset = start + 4;
    for (size_t i = 0; i < blocks; ++i) {
        ASSERT_EQ(wire.compare(offset, chunk.size(), chunk), 0) << i;
        offset += chunk.size();
    }
    EXPECT_EQ(wire.substr(offset), "0\r\n\r\n");
}

int main(int argc, char **argv) {
    test_executable = argv[0];
    if (is_signal_helper(argc, argv))
        return run_signal_server(
            static_cast<uint16_t>(std::strtoul(argv[2], nullptr, 10)));
    ::testing::InitGoogleTest(&argc, argv);

    return RUN_ALL_TESTS();
}
