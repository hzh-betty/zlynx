#include "../test_support.h"
#include "zhttp/protocol/http_protocol_handler.h"
#include "zhttp/zhttp_logger.h"
#include "znet/socket.h"
#include "znet/tcp_server.h"
#include <limits>
#include <sys/socket.h>
#include <unistd.h>
using namespace zhttp;
namespace {
struct SocketPair {
    SocketPair() : runtime(zco::RuntimeOptions{1}) {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
            throw std::runtime_error("socketpair");
        connection = std::make_shared<znet::TcpConnection>(
            std::make_shared<znet::Socket>(fds[0]), runtime.executor(0));
        peer = fds[1];
    }
    ~SocketPair() {
        connection->close();
        ::close(peer);
    }
    std::string read() {
        std::string bytes;
        char data[8192];
        ssize_t n;
        while ((n = ::recv(peer, data, sizeof(data), MSG_DONTWAIT)) > 0)
            bytes.append(data, n);
        return bytes;
    }

    zco::Runtime runtime;
    znet::TcpConnection::ptr connection;
    int peer;
};
class ServerAccess : public HttpServer {
  public:
    ServerAccess()
        : HttpServer(std::make_shared<znet::IPv4Address>("127.0.0.1", 0),
                     zco::RuntimeOptions{1}) {}
    using HttpServer::tcp_server;
};
TEST(HttpServerContextTest, TimeoutSettersClampAndInvalidTlsIsRejected) {
    ServerAccess server;
    server.set_recv_timeout(std::numeric_limits<uint64_t>::max());
    server.set_write_timeout(std::numeric_limits<uint64_t>::max());
    server.set_keepalive_timeout(123456);
    EXPECT_EQ(server.tcp_server()->read_timeout(),
              std::numeric_limits<uint32_t>::max() - 1);
    EXPECT_EQ(server.tcp_server()->write_timeout(),
              std::numeric_limits<uint32_t>::max() - 1);
    EXPECT_EQ(server.tcp_server()->keepalive_timeout(), 123456u);
    EXPECT_FALSE(server.set_ssl_certificate("/missing/cert", "/missing/key"));
}
TEST(HttpWriterTest, SynchronousStreamFinishesBeforeSendReturns) {
    SocketPair pair;
    auto request = std::make_shared<HttpRequest>();
    HttpContext context(request);
    context.response().stream([remaining = 1](char *buffer, size_t) mutable {
        if (!remaining--)
            return size_t{0};
        buffer[0] = 'o';
        buffer[1] = 'k';
        return size_t{2};
    });
    EXPECT_EQ(HttpResponseWriter::send(pair.connection, context),
              WriteResult::Completed);
    EXPECT_NE(pair.read().find("2\r\nok\r\n0\r\n\r\n"), std::string::npos);
}
TEST(HttpWriterTest, DeclaredStreamLengthMustMatchExactly) {
    for (size_t length : {1U, 2U, 3U}) {
        SocketPair pair;
        HttpContext context(std::make_shared<HttpRequest>());
        context.response().stream([remaining = 1](char *buffer, size_t) mutable {
            if (!remaining--)
                return size_t{0};
            buffer[0] = 'a';
            buffer[1] = 'b';
            return size_t{2};
        }, length);
        EXPECT_EQ(HttpResponseWriter::send(pair.connection, context),
                  length == 2 ? WriteResult::Completed : WriteResult::Failed);
        const auto wire = pair.read();
        EXPECT_EQ(wire.find("Transfer-Encoding"), std::string::npos);
        if (length == 2)
            EXPECT_EQ(wire.substr(wire.find("\r\n\r\n") + 4), "ab");
    }
}
TEST(HttpWriterTest, InvalidStreamDoesNotEmitTerminalChunk) {
    for (bool throws : {false, true}) {
        SocketPair pair;
        HttpContext context(std::make_shared<HttpRequest>());
        context.response().stream([throws](char *, size_t capacity) -> size_t {
            if (throws)
                throw std::runtime_error("stream failed");
            return capacity + 1;
        });
        EXPECT_EQ(HttpResponseWriter::send(pair.connection, context),
                  WriteResult::Failed);
        const auto wire = pair.read();
        EXPECT_EQ(wire.substr(wire.find("\r\n\r\n") + 4), "");
    }
}
TEST(HttpWriterTest, SlowClientStreamFailsWithinWriteTimeout) {
    SocketPair pair;
    int size = 1024;
    ASSERT_EQ(::setsockopt(pair.connection->fd(), SOL_SOCKET, SO_SNDBUF, &size,
                           sizeof(size)), 0);
    pair.connection->set_write_timeout(20);
    HttpContext context(std::make_shared<HttpRequest>());
    context.response().stream([](char *buffer, size_t capacity) {
        std::memset(buffer, 'x', capacity);
        return capacity;
    });
    const auto begin = std::chrono::steady_clock::now();
    EXPECT_EQ(HttpResponseWriter::send(pair.connection, context),
              WriteResult::Failed);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - begin).count(), 500);
}
TEST(HttpProtocolTest, SynchronousStreamProcessesAlreadyBufferedNextRequest) {
    SocketPair pair;
    Router router;
    RequestPipeline pipeline;
    int next = 0, completed = 0;
    router.get("/stream", [&](HttpContext &context) {
        context.on_complete([&](CompletionResult result) {
            EXPECT_EQ(result, CompletionResult::Completed);
            ++completed;
        });
        context.response().stream([](char *, size_t) { return 0U; });
    });
    router.get("/next", [&](HttpContext &context) {
        EXPECT_EQ(completed, 1);
        ++next;
        context.response().text("next");
    });
    HttpProtocolHandler handler(router, pipeline, {}, 30000, "test", {});
    auto &buffer = pair.connection->input_buffer();
    buffer.append("GET /stream HTTP/1.1\r\nHost: test\r\n\r\nGET /next "
                  "HTTP/1.1\r\nHost: test\r\n\r\n");
    handler.on_data(pair.connection, buffer);
    EXPECT_EQ(next, 1);
    EXPECT_EQ(buffer.readable_bytes(), 0U);
    EXPECT_NE(pair.read().find("0\r\n\r\nHTTP/1.1 200 OK"), std::string::npos);
}
TEST(HttpProtocolTest, ExpectIsRejectedBeforeBodyArrives) {
    SocketPair pair;
    Router router;
    RequestPipeline pipeline;
    int calls = 0;
    router.post("/", [&](HttpContext &) { ++calls; });
    HttpProtocolHandler handler(router, pipeline, {}, 30000, "test", {});
    znet::Buffer buffer;
    buffer.append("POST / HTTP/1.1\r\nHost: test\r\nExpect: "
                  "100-continue\r\nContent-Length: 100\r\n\r\n");
    handler.on_data(pair.connection, buffer);
    EXPECT_EQ(calls, 0);
    EXPECT_NE(pair.read().find("417 Expectation Failed"), std::string::npos);
    EXPECT_FALSE(pair.connection->connected());
}
TEST(HttpWriterTest,
     SuppressedBodiesDoNotStartProducersAndHttp10ClosesUnknownLength) {
    for (auto code : {200, 204, 205, 304}) {
        SocketPair pair;
        auto request = std::make_shared<HttpRequest>();
        request->set_method(code == 200 ? HttpMethod::HEAD : HttpMethod::GET);
        HttpContext context(request);
        int calls = 0;
        context.response().status(code).stream(
            [&](char *, size_t) { ++calls; return 0U; });
            EXPECT_EQ(HttpResponseWriter::send(pair.connection, context),
                  WriteResult::Completed);
        EXPECT_EQ(calls, 0);
        EXPECT_EQ(pair.read().find("Transfer-Encoding"), std::string::npos);
    }
    auto request = std::make_shared<HttpRequest>();
    request->set_version(HttpVersion::HTTP_1_0);
    request->set_method(HttpMethod::GET);
    HttpContext context(request);
    context.response().stream([](char *, size_t) { return 0U; });
    const auto headers = HttpResponseWriter::headers(request->request_line(),
                                                     context.response());
    EXPECT_NE(headers.find("Connection: close"), std::string::npos);
    EXPECT_EQ(headers.find("Transfer-Encoding"), std::string::npos);
}
} // namespace

TEST(HttpWriterTest, FileOwnsResourceAndIsCheckedBeforeCommit) {
    char path[] = "/tmp/zhttp-body-XXXXXX";
    const int fd = ::mkstemp(path);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::write(fd, "abcdef", 6), 6);
    SocketPair pair;
    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    HttpContext context(request);
    context.response().body(HttpBody::file(path, 2, 3));
    ::unlink(path);
    ::close(fd);
    EXPECT_EQ(HttpResponseWriter::send(pair.connection, context),
              WriteResult::Completed);
    const auto wire = pair.read();
    EXPECT_NE(wire.find("Content-Length: 3\r\n"), std::string::npos);
    EXPECT_EQ(wire.substr(wire.find("\r\n\r\n") + 4), "cde");
    char truncated[] = "/tmp/zhttp-body-XXXXXX";
    const int second = ::mkstemp(truncated);
    ASSERT_GE(second, 0);
    ASSERT_EQ(::write(second, "abc", 3), 3);
    HttpContext invalid(request);
    invalid.response().body(HttpBody::file(truncated));
    ASSERT_EQ(::ftruncate(second, 0), 0);
    ::unlink(truncated);
    ::close(second);
    EXPECT_THROW(HttpResponseWriter::send(pair.connection, invalid), std::runtime_error);
    EXPECT_FALSE(invalid.response().committed());
    EXPECT_TRUE(pair.read().empty());
}
TEST(HttpWriterTest, FreezesResponseAndNormalizesFraming) {
    SocketPair pair;
    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    HttpContext context(request);
    context.response()
        .status(599)
        .header("Content-Length", "100")
        .append_header("Transfer-Encoding", "gzip")
        .body("abc");
    EXPECT_EQ(HttpResponseWriter::send(pair.connection, context),
              WriteResult::Completed);
    const auto wire = pair.read();
    EXPECT_EQ(wire.find("HTTP/1.1 599 \r\n"), 0u);
    EXPECT_NE(wire.find("Content-Length: 3\r\n"), std::string::npos);
    EXPECT_EQ(wire.find("Transfer-Encoding"), std::string::npos);
    EXPECT_THROW(context.response().text("late"), std::logic_error);
    EXPECT_THROW(context.response().header("Late", "value"), std::logic_error);
    HttpResponse reset;
    reset.status(205).stream([](char *, size_t) { return 0U; });
    EXPECT_NE(
        HttpResponseWriter::serialize(reset).find("Content-Length: 0\r\n"),
        std::string::npos);
    EXPECT_THROW(HttpContext(nullptr), std::invalid_argument);
}
TEST(RouterMatchTest, ReturnsStablePatternWithoutExecutingBusinessCode) {
    Router router;
    int calls = 0;
    router.get("/users/:id", [&](HttpContext &) { ++calls; });
    auto match = router.match("/users/42", HttpMethod::GET);
    ASSERT_TRUE(match.found);
    EXPECT_EQ(match.route_id, "/users/:id");
    EXPECT_EQ(match.params.at("id"), "42");
    EXPECT_EQ(calls, 0);
    router.add_regex_route(HttpMethod::GET, "^/files/(.*)$", {"name"},
                           [](HttpContext &) {});
    EXPECT_EQ(router.match("/files/readme", HttpMethod::GET).route_id,
              "^/files/(.*)$");
    router.freeze();
    EXPECT_THROW(router.get("/late", [](HttpContext &) {}), std::logic_error);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    zhttp::init_logger();
    return RUN_ALL_TESTS();
}
