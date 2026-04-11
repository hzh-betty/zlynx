#include "zhttp/http_server.h"
#include "zhttp/zhttp_logger.h"
#include "zhttp/http_server_builder.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace zhttp;
using namespace zhttp::mid;

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

    const std::string path(buffer.data());
    std::ofstream output(path, std::ios::binary);
    output << pem_content;
    output.close();
    ::close(fd);

    return output.good() ? path : "";
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

class ScopedServer {
  public:
    explicit ScopedServer(std::shared_ptr<HttpServer> server)
        : server_(std::move(server)) {}

    ~ScopedServer() {
        if (server_) {
            server_->stop();
        }
    }

  private:
    std::shared_ptr<HttpServer> server_;
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

int connect_with_retry(uint16_t port, int retry_count, int retry_delay_ms) {
    for (int attempt = 0; attempt < retry_count; ++attempt) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) ==
            0) {
            return fd;
        }

        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
    }

    return -1;
}

bool send_all(int fd, const std::string &data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::string recv_until_close(int fd, int timeout_ms) {
    std::string response;
    char buffer[1024];

    while (true) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN | POLLHUP;

        const int rc = ::poll(&pfd, 1, timeout_ms);
        if (rc <= 0) {
            break;
        }

        const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;
        }

        response.append(buffer, static_cast<size_t>(n));
    }

    return response;
}

} // namespace

// 注意：集成测试需要网络环境，可能需要特殊配置
// 这里提供基本的路由和中间件测试

TEST(HttpServerIntegrationTest, RouteRegistration) {
    bool handler_called = false;
    Router router;

    router.get("/test",
               [&handler_called](const HttpRequest::ptr &, HttpResponse &resp) {
                   handler_called = true;
                   resp.status(HttpStatus::OK).text("OK");
               });

    // 模拟路由匹配
    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    request->set_path("/test");
    HttpResponse response;

    bool found = router.route(request, response);

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

        bool before(const HttpRequest::ptr &, HttpResponse &resp) override {
            before_called_ = true;
            resp.header("X-Test-Middleware", "before");
            return true;
        }

        void after(const HttpRequest::ptr &, HttpResponse &resp) override {
            after_called_ = true;
            resp.header("X-Test-After", "after");
        }

      private:
        bool &before_called_;
        bool &after_called_;
    };

    bool before_called = false;
    bool after_called = false;
    Router router;

    router.use(std::make_shared<TestMiddleware>(before_called, after_called));
    router.get("/middleware-test",
               [](const HttpRequest::ptr &, HttpResponse &resp) {
                   resp.status(HttpStatus::OK).text("OK");
               });

    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    request->set_path("/middleware-test");
    HttpResponse response;

    router.route(request, response);

    EXPECT_TRUE(before_called);
    EXPECT_TRUE(after_called);
    EXPECT_EQ(response.headers().at("X-Test-Middleware"), "before");
    EXPECT_EQ(response.headers().at("X-Test-After"), "after");
}

TEST(HttpServerIntegrationTest, NotFoundRoute) {
    Router router;
    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    request->set_path("/nonexistent");
    HttpResponse response;

    bool found = router.route(request, response);

    EXPECT_FALSE(found);
    EXPECT_EQ(response.status_code(), HttpStatus::NOT_FOUND);
}

TEST(HttpServerIntegrationTest, KeepsParserStateAcrossSplitPackets) {
    const uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    HttpServerBuilder builder;
    builder.listen("127.0.0.1", port)
        .threads(1)
        .log_level("error")
        .get("/split", [](const HttpRequest::ptr &, HttpResponse &resp) {
            resp.status(HttpStatus::OK).text("split-ok");
        });

    auto server = builder.build();
    ASSERT_TRUE(server);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());

    const int client_fd = connect_with_retry(port, 20, 25);
    ASSERT_GE(client_fd, 0);

    const std::string first_chunk = "GET /split HTTP/1.1\r\n"
                                    "Host: localhost\r\n"
                                    "Connection: close";
    const std::string second_chunk = "\r\n\r\n";

    ASSERT_TRUE(send_all(client_fd, first_chunk));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    ASSERT_TRUE(send_all(client_fd, second_chunk));

    const std::string response = recv_until_close(client_fd, 1000);
    ::close(client_fd);

    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos) << response;
    EXPECT_NE(response.find("split-ok"), std::string::npos) << response;
}

TEST(HttpServerIntegrationTest, HandlesChunkedRequestBodyEndToEnd) {
    const uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    HttpServerBuilder builder;
    builder.listen("127.0.0.1", port)
        .threads(1)
        .log_level("error")
        .post("/upload", [](const HttpRequest::ptr &req, HttpResponse &resp) {
            resp.status(HttpStatus::OK).text(req->body());
        });

    auto server = builder.build();
    ASSERT_TRUE(server);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());

    const int client_fd = connect_with_retry(port, 20, 25);
    ASSERT_GE(client_fd, 0);

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
    ::close(client_fd);

    // 服务端会先把 chunk 合并成完整 body，再由业务逻辑回显。
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos) << response;
    EXPECT_NE(response.find("\r\n\r\nWikipedia"), std::string::npos)
        << response;
}

TEST(HttpServerIntegrationTest, SendsExplicitChunkedResponseBody) {
    const uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    HttpServerBuilder builder;
    builder.listen("127.0.0.1", port)
        .threads(1)
        .log_level("error")
        .get("/chunked-static",
             [](const HttpRequest::ptr &, HttpResponse &resp) {
                 resp.status(HttpStatus::OK)
                     .content_type("text/plain")
                     .body("hello")
                     .enable_chunked();
             });

    auto server = builder.build();
    ASSERT_TRUE(server);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());

    const int client_fd = connect_with_retry(port, 20, 25);
    ASSERT_GE(client_fd, 0);

    const std::string request = "GET /chunked-static HTTP/1.1\r\n"
                                "Host: localhost\r\n"
                                "Connection: close\r\n"
                                "\r\n";

    ASSERT_TRUE(send_all(client_fd, request));
    const std::string response = recv_until_close(client_fd, 1000);
    ::close(client_fd);

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
    const uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    HttpServerBuilder builder;
    builder.listen("127.0.0.1", port)
        .threads(1)
        .log_level("error")
        .get("/chunked-stream", [](const HttpRequest::ptr &,
                                   HttpResponse &resp) {
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

    auto server = builder.build();
    ASSERT_TRUE(server);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());

    const int client_fd = connect_with_retry(port, 20, 25);
    ASSERT_GE(client_fd, 0);

    const std::string request = "GET /chunked-stream HTTP/1.1\r\n"
                                "Host: localhost\r\n"
                                "Connection: close\r\n"
                                "\r\n";

    ASSERT_TRUE(send_all(client_fd, request));
    const std::string response = recv_until_close(client_fd, 1000);
    ::close(client_fd);

    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos) << response;
    EXPECT_NE(response.find("Transfer-Encoding: chunked"), std::string::npos)
        << response;
    EXPECT_EQ(response.find("Content-Length:"), std::string::npos) << response;
    EXPECT_NE(response.find("\r\n\r\n4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n"),
              std::string::npos)
        << response;
}

TEST(HttpServerIntegrationTest, SendsAsyncChunkedStreamResponse) {
    const uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    HttpServerBuilder builder;
    builder.listen("127.0.0.1", port)
        .threads(1)
        .log_level("error")
        .get("/chunked-async", [](const HttpRequest::ptr &,
                                  HttpResponse &resp) {
            resp.status(HttpStatus::OK)
                .content_type("text/plain")
                .async_stream([](HttpResponse::AsyncChunkSender send,
                                 HttpResponse::AsyncStreamCloser close) {
                    // 异步推送两段数据，最终通过 close() 触发终止块发送。
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    if (!send("Wiki")) {
                        close();
                        return;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    (void)send("pedia");
                    close();
                });
        });

    auto server = builder.build();
    ASSERT_TRUE(server);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());

    const int client_fd = connect_with_retry(port, 20, 25);
    ASSERT_GE(client_fd, 0);

    const std::string request = "GET /chunked-async HTTP/1.1\r\n"
                                "Host: localhost\r\n"
                                "Connection: keep-alive\r\n"
                                "\r\n";

    ASSERT_TRUE(send_all(client_fd, request));
    const std::string response = recv_until_close(client_fd, 2000);
    ::close(client_fd);

    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos) << response;
    EXPECT_NE(response.find("Transfer-Encoding: chunked"), std::string::npos)
        << response;
    EXPECT_NE(response.find("Connection: close"), std::string::npos)
        << response;
    EXPECT_EQ(response.find("Content-Length:"), std::string::npos) << response;
    EXPECT_NE(response.find("\r\n\r\n4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n"),
              std::string::npos)
        << response;
}

TEST(HttpServerIntegrationTest, HttpsRoundTripWithRealTlsHandshake) {
    const uint16_t port = find_free_port();
    ASSERT_NE(port, 0);

    ScopedTlsPemFiles pem_files;
    ASSERT_FALSE(pem_files.cert_path().empty());
    ASSERT_FALSE(pem_files.key_path().empty());

    HttpServerBuilder builder;
    builder.listen("127.0.0.1", port)
        .threads(1)
        .log_level("error")
        .enable_https(pem_files.cert_path(), pem_files.key_path())
        .get("/secure", [](const HttpRequest::ptr &, HttpResponse &resp) {
            resp.status(HttpStatus::OK).text("secure-ok");
        });

    auto server = builder.build();
    ASSERT_TRUE(server);
    ScopedServer guard(server);
    ASSERT_TRUE(server->start());

    const int client_fd = connect_with_retry(port, 20, 25);
    ASSERT_GE(client_fd, 0);

    SSL_CTX *ssl_ctx = SSL_CTX_new(TLS_client_method());
    ASSERT_NE(ssl_ctx, nullptr);
    SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, nullptr);

    SSL *ssl = SSL_new(ssl_ctx);
    ASSERT_NE(ssl, nullptr);
    ASSERT_EQ(SSL_set_fd(ssl, client_fd), 1);
    ASSERT_EQ(SSL_connect(ssl), 1);

    const std::string request = "GET /secure HTTP/1.1\r\n"
                                "Host: localhost\r\n"
                                "Connection: close\r\n\r\n";

    size_t sent = 0;
    while (sent < request.size()) {
        const int n = SSL_write(ssl, request.data() + sent,
                                static_cast<int>(request.size() - sent));
        ASSERT_GT(n, 0);
        sent += static_cast<size_t>(n);
    }

    std::string response;
    char read_buffer[1024];
    while (true) {
        const int n = SSL_read(ssl, read_buffer, sizeof(read_buffer));
        if (n > 0) {
            response.append(read_buffer, static_cast<size_t>(n));
            continue;
        }

        const int ssl_error = SSL_get_error(ssl, n);
        if (ssl_error == SSL_ERROR_ZERO_RETURN) {
            break;
        }
        ASSERT_NE(ssl_error, SSL_ERROR_SSL);
        ASSERT_NE(ssl_error, SSL_ERROR_SYSCALL);
        if (ssl_error == SSL_ERROR_WANT_READ ||
            ssl_error == SSL_ERROR_WANT_WRITE) {
            continue;
        }
        break;
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ssl_ctx);
    ::close(client_fd);

    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos) << response;
    EXPECT_NE(response.find("secure-ok"), std::string::npos) << response;
}

int main(int argc, char **argv) {
    zhttp::init_logger();
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
