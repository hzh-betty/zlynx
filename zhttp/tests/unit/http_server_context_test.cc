#include "protocol/http/response_encoder.h"
#include "../support/network_fixture.h"
#include "protocol/http/http_protocol_handler.h"
#include "znet/transport/socket.h"
#include "znet/server/tcp_server.h"
#include <limits>
#include <fcntl.h>
#include <filesystem>
#include <sys/socket.h>
#include <unistd.h>
using namespace zhttp;
namespace {
struct SocketPair {
    explicit SocketPair(std::chrono::milliseconds timeout = {}) : runtime(zco::RuntimeOptions{1}) {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
            throw std::runtime_error("socketpair");
        connection = std::make_shared<znet::Connection>(
            std::move(znet::make_tcp_stream(std::move(znet::Socket::adopt(fds[0])).value())).value(),
            runtime.executor(0), timeout);
        connection->start().value();
        peer = fds[1];
    }
    ~SocketPair() {
        (void)connection->close();
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
    znet::Connection::ptr connection;
    int peer;
};
TEST(HttpServerContextTest, TimeoutSettersClampAndInvalidTlsIsRejected) {
    HttpServer server(znet::Endpoint::ipv4("127.0.0.1", 0).value(), zco::RuntimeOptions{1});
    server.set_recv_timeout(std::numeric_limits<uint64_t>::max());
    server.set_write_timeout(std::numeric_limits<uint64_t>::max());
    server.set_keepalive_timeout(123456);
    EXPECT_FALSE(server.set_ssl_certificate("/missing/cert", "/missing/key"));
    EXPECT_TRUE(server.start());
    EXPECT_THROW(server.set_recv_timeout(1), std::logic_error);
    server.stop();
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
    SocketPair pair(std::chrono::milliseconds{20});
    int size = 1024;
    ASSERT_EQ(::setsockopt(pair.connection->native_handle(), SOL_SOCKET, SO_SNDBUF, &size,
                           sizeof(size)), 0);
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
    HttpApplication application;
    auto &router = application.router();
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
    HttpProtocolHandler handler(application, {}, 30000, "test", {});
    znet::ByteBuffer buffer;
    buffer.append("GET /stream HTTP/1.1\r\nHost: test\r\n\r\nGET /next "
                  "HTTP/1.1\r\nHost: test\r\n\r\n");
    handler.on_data(pair.connection, buffer);
    EXPECT_EQ(next, 1);
    EXPECT_EQ(buffer.readable_bytes(), 0U);
    EXPECT_NE(pair.read().find("0\r\n\r\nHTTP/1.1 200 OK"), std::string::npos);
}
TEST(HttpProtocolTest, ExpectIsRejectedBeforeBodyArrives) {
    SocketPair pair;
    HttpApplication application;
    auto &router = application.router();
    int calls = 0;
    router.post("/", [&](HttpContext &) { ++calls; });
    HttpProtocolHandler handler(application, {}, 30000, "test", {});
    znet::ByteBuffer buffer;
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
    const auto headers = ResponseEncoder::headers(request->request_line(),
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
        ResponseEncoder::serialize(reset).find("Content-Length: 0\r\n"),
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

    return RUN_ALL_TESTS();
}

namespace {
std::string fetch(HttpServer &server, const std::string &path) {
    const auto endpoint = server.local_endpoint().value();
    auto socket = znet::Socket::adopt(::socket(endpoint.family(), SOCK_STREAM, 0)).value();
    const int flags = ::fcntl(socket.native_handle(), F_GETFL, 0);
    ::fcntl(socket.native_handle(), F_SETFL, flags & ~O_NONBLOCK);
    if (::connect(socket.native_handle(), endpoint.native_address(), endpoint.native_size()) != 0)
        throw std::runtime_error("connect");
    timeval timeout{2, 0};
    ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    const std::string request = "GET " + path + " HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
    if (::send(socket.native_handle(), request.data(), request.size(), MSG_NOSIGNAL) !=
        static_cast<ssize_t>(request.size()))
        throw std::runtime_error("send");
    std::string result;
    char buffer[4096];
    ssize_t count;
    while ((count = ::recv(socket.native_handle(), buffer, sizeof(buffer), 0)) > 0)
        result.append(buffer, count);
    return result;
}
TEST(HttpServerOwnershipTest, SharedRuntimeAndApplicationOutliveIndependentListeners) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    auto application = std::make_shared<HttpApplication>();
    application->router().get("/items/:id", [](HttpContext &context) {
        context.response().text(context.path_param("id"));
    });
    auto endpoint = znet::Endpoint::ipv4("127.0.0.1", 0).value();
    HttpServer first(endpoint, runtime, application);
    HttpServer second(endpoint, runtime, application);
    ASSERT_TRUE(first.start());
    ASSERT_TRUE(second.start());
    application.reset(); // Listeners and session factories retain the application.
    EXPECT_NE(fetch(first, "/items/1").find("\r\n\r\n1"), std::string::npos);
    EXPECT_NE(fetch(second, "/items/2").find("\r\n\r\n2"), std::string::npos);
    first.stop();
    EXPECT_NE(fetch(second, "/items/3").find("\r\n\r\n3"), std::string::npos);
    ASSERT_TRUE(first.start());
    EXPECT_FALSE(first.stop_requested());
    EXPECT_NE(fetch(first, "/items/4").find("\r\n\r\n4"), std::string::npos);
    first.stop();
    second.stop();
    EXPECT_TRUE(runtime.spawn([] {}).value().join());
}
TEST(HttpServerOwnershipTest, RequestCallbackStopsWithoutJoiningItself) {
    HttpServer server(znet::Endpoint::ipv4("127.0.0.1", 0).value(), zco::RuntimeOptions{1});
    std::atomic<bool> returned{false};
    server.router().get("/stop", [&](HttpContext &) {
        server.request_stop();
        returned.store(true);
    });
    ASSERT_TRUE(server.start());
    (void)fetch(server, "/stop");
    server.stop();
    EXPECT_TRUE(returned.load());
    EXPECT_TRUE(server.stop_requested());
    EXPECT_FALSE(server.is_running());
}
TEST(HttpServerOwnershipTest, StartupFailureRetainsErrorAndFreezesRegistration) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    HttpServer first(znet::Endpoint::ipv4("127.0.0.1", 0).value(), runtime);
    ASSERT_TRUE(first.start());
    HttpServer conflicting(first.local_endpoint().value(), runtime);
    auto started = conflicting.start();
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error().code, std::make_error_code(std::errc::address_in_use));
    EXPECT_THROW(conflicting.router().get("/late", [](HttpContext &) {}), std::logic_error);
    HttpServer tls(znet::Endpoint::ipv4("127.0.0.1", 0).value(), runtime);
    auto loaded = tls.set_ssl_certificate("/missing/cert", "/missing/key");
    ASSERT_FALSE(loaded);
    EXPECT_FALSE(loaded.error().message().empty());
}
} // namespace

TEST(HttpServerOwnershipTest, OfflineServerConstructionDoesNotCreateWorkers) {
    auto thread_count = [] {
        return static_cast<size_t>(std::distance(
            std::filesystem::directory_iterator("/proc/self/task"), std::filesystem::directory_iterator{}));
    };
    const auto before = thread_count();
    HttpServer server(znet::Endpoint::ipv4("127.0.0.1", 0).value(), zco::RuntimeOptions{8});
    server.router().get("/offline", [](HttpContext &context) { context.response().text("offline"); });
    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    request->set_path("/offline");
    HttpContext context(request);
    EXPECT_TRUE(server.handle(context));
    EXPECT_EQ(context.response().body_content(), "offline");
    EXPECT_EQ(thread_count(), before);
}
