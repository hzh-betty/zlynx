#include "../support/network_fixture.h"
#include "../support/temp_file.h"
#include "protocol/http/http_protocol_handler.h"
#include "protocol/http/response_encoder.h"
#include "znet/server/tcp_server.h"
#include "znet/transport/socket.h"
#include <filesystem>
#include <limits>
#include <sys/socket.h>
#include <unistd.h>
using namespace zhttp;
namespace {
struct SocketPair {
    explicit SocketPair(std::chrono::milliseconds timeout = {}) : runtime(zco::RuntimeOptions{1}) {
        int fds[2];
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
            throw std::runtime_error("socketpair");
        try {
            peer = zco::io::Descriptor(fds[1]);
        } catch (...) {
            ::close(fds[0]);
            throw;
        }
        connection = std::make_shared<znet::Connection>(
            std::move(znet::make_tcp_stream(std::move(znet::Socket::adopt(fds[0])).value())).value(),
            runtime.executor(0), timeout);
        connection->start().value();
    }

    ~SocketPair() { (void)connection->close(); }
    std::string read() {
        std::string bytes;
        char data[8192];
        ssize_t n;
        while ((n = ::recv(peer.native_handle(), data, sizeof(data),
                           MSG_DONTWAIT)) > 0)
            bytes.append(data, n);
        return bytes;
    }

    zco::Runtime runtime;
    znet::Connection::ptr connection;
    zco::io::Descriptor peer;
};
struct RecordingStream : znet::ByteStream {
    std::string wire;
    size_t writes = 0, max_write = std::numeric_limits<size_t>::max();
    size_t fail_after = std::numeric_limits<size_t>::max();
    mutable int local_queries = 0, remote_queries = 0;
    bool missing_endpoints = false, tls = false;
    znet::Result<void> start(zco::Deadline) override { return {}; }
    znet::Transfer read_some(void *, size_t, zco::Deadline) override { return {}; }
    znet::Transfer write_some(const void *data, size_t size, zco::Deadline) override {
        ++writes;
        if (wire.size() >= fail_after)
            return {}; // 无进展：Connection::send 必须关闭连接并返回失败。
        const auto count = std::min({size, max_write, fail_after - wire.size()});
        wire.append(static_cast<const char *>(data), count);
        return {count, {}, false};
    }
    znet::Result<void> shutdown_write(zco::Deadline) override { return {}; }
    znet::Result<void> close() override { return {}; }
    znet::Result<znet::Endpoint> local_endpoint() const override {
        ++local_queries;
        return znet::Endpoint::ipv4(missing_endpoints ? "invalid" : "127.0.0.1", 8080);
    }
    znet::Result<znet::Endpoint> remote_endpoint() const override {
        ++remote_queries;
        return znet::Endpoint::ipv4(missing_endpoints ? "invalid" : "127.0.0.2", 9000);
    }
    int native_handle() const override { return -1; }
    bool encrypted() const override { return tls; }
};
struct RecordingConnection {
    zco::Runtime runtime{zco::RuntimeOptions{1}};
    RecordingStream *stream;
    znet::Connection::ptr connection;
    RecordingConnection() {
        auto transport = std::make_unique<RecordingStream>();
        stream = transport.get();
        connection = std::make_shared<znet::Connection>(std::move(transport), runtime.executor(0));
        connection->start().value();
    }
};
TEST(HttpProtocolTest, ConnectionMetadataIsOwnedAndQueriedOnceAcrossRequests) {
    for (bool missing : {false, true}) {
        RecordingConnection pair;
        pair.stream->missing_endpoints = missing;
        pair.stream->tls = true;
        HttpApplication application;
        std::vector<ConnectionInfo> snapshots;
        application.router().get("/", [&](HttpContext &context) {
            snapshots.push_back(context.connection_info());
            context.response().text("OK");
        });
        HttpProtocolHandler handler(application, {}, 30000, "test", {});
        znet::ByteBuffer buffer;
        for (int i = 0; i < 3; ++i) {
            buffer.append("GET / HTTP/1.1\r\nHost: test\r\n\r\n");
            handler.on_data(pair.connection, buffer);
        }
        handler.on_closed();
        ASSERT_EQ(snapshots.size(), 3U);
        EXPECT_EQ(pair.stream->local_queries, 1);
        EXPECT_EQ(pair.stream->remote_queries, 1);
        for (const auto &snapshot : snapshots) {
            EXPECT_TRUE(snapshot.tls);
            EXPECT_EQ(snapshot.remote_address, missing ? "" : "127.0.0.2:9000");
            EXPECT_EQ(snapshot.local_address, missing ? "" : "127.0.0.1:8080");
        }
    }
}
TEST(HttpWriterTest, CombinesOnlySmallUnchunkedMemoryBodies) {
    for (size_t size : {0U, 2U, 4096U, 4097U}) {
        RecordingConnection pair;
        HttpContext context(std::make_shared<HttpRequest>());
        context.response().body(std::string(size, 'x'));
        const auto expected = ResponseEncoder::serialize(context.response());
        EXPECT_EQ(HttpResponseWriter::send(pair.connection, context), WriteResult::Completed);
        EXPECT_EQ(pair.stream->wire, expected);
        EXPECT_EQ(pair.stream->writes, size <= 4096 ? 1U : 2U);
    }
    for (int code : {200, 204, 205, 304, 101}) {
        RecordingConnection pair;
        auto request = std::make_shared<HttpRequest>();
        request->set_method(code == 200 ? HttpMethod::HEAD : HttpMethod::GET);
        HttpContext context(request);
        context.response().status(code).body("hidden");
        const auto expected = ResponseEncoder::serialize(context.response(), true, request->method());
        EXPECT_EQ(HttpResponseWriter::send(pair.connection, context, code == 101), WriteResult::Completed);
        EXPECT_EQ(pair.stream->wire, expected);
        EXPECT_EQ(pair.stream->writes, 1U);
        EXPECT_EQ(pair.stream->wire.find("\r\n\r\nhidden"), std::string::npos);
    }
}
TEST(HttpWriterTest, CombinedBufferSurvivesPartialWritesAndReportsFailure) {
    for (bool fail : {false, true}) {
        RecordingConnection pair;
        pair.stream->max_write = 7;
        if (fail) pair.stream->fail_after = 23;
        HttpContext context(std::make_shared<HttpRequest>());
        context.response().text("OK");
        const auto expected = ResponseEncoder::serialize(context.response());
        EXPECT_EQ(HttpResponseWriter::send(pair.connection, context),
                  fail ? WriteResult::Failed : WriteResult::Completed);
        EXPECT_TRUE(context.response().committed());
        EXPECT_EQ(pair.stream->wire, fail ? expected.substr(0, 23) : expected);
        EXPECT_EQ(pair.connection->connected(), !fail);
    }
}
TEST(HttpProtocolTest, PartialCombinedWriteFailsCompletionAndClosesPipeline) {
    RecordingConnection pair;
    pair.stream->max_write = 7;
    pair.stream->fail_after = 23;
    HttpApplication application;
    int calls = 0, failures = 0;
    application.router().get("/", [&](HttpContext &context) {
        ++calls;
        context.on_complete([&](CompletionResult result) {
            if (result == CompletionResult::Failed) ++failures;
        });
        context.response().text("OK");
    });
    HttpProtocolHandler handler(application, {}, 30000, "test", {});
    znet::ByteBuffer buffer;
    buffer.append("GET / HTTP/1.1\r\nHost: test\r\n\r\nGET / HTTP/1.1\r\nHost: test\r\n\r\n");
    handler.on_data(pair.connection, buffer);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(failures, 1);
    EXPECT_FALSE(pair.connection->connected());
}
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
    test::TempFile file;
    const auto &path = file.path;
    const int fd = file.descriptor.native_handle();
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::write(fd, "abcdef", 6), 6);
    SocketPair pair;
    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    HttpContext context(request);
    context.response().body(HttpBody::file(path, 2, 3));
    ::unlink(path.c_str());
    file.descriptor.close().value();
    EXPECT_EQ(HttpResponseWriter::send(pair.connection, context),
              WriteResult::Completed);
    const auto wire = pair.read();
    EXPECT_NE(wire.find("Content-Length: 3\r\n"), std::string::npos);
    EXPECT_EQ(wire.substr(wire.find("\r\n\r\n") + 4), "cde");
    test::TempFile other;
    const auto &truncated = other.path;
    const int second = other.descriptor.native_handle();
    ASSERT_GE(second, 0);
    ASSERT_EQ(::write(second, "abc", 3), 3);
    HttpContext invalid(request);
    invalid.response().body(HttpBody::file(truncated));
    ASSERT_EQ(::ftruncate(second, 0), 0);
    ::unlink(truncated.c_str());
    other.descriptor.close().value();
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

namespace {
std::string fetch(HttpServer &server, const std::string &path) {
    const auto endpoint = server.local_endpoint().value();
    zco::io::Descriptor socket(::socket(endpoint.family(), SOCK_STREAM, 0));
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
