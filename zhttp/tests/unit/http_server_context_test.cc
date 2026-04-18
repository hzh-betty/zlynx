#include "zhttp/http_server.h"

#include "zhttp/zhttp_logger.h"

#include "zco/sched.h"
#include "znet/socket.h"

#include <gtest/gtest.h>
#include <csignal>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <sys/socket.h>
#include <unistd.h>

namespace zhttp {
namespace {

class HttpServerContextTestDouble : public HttpServer {
  public:
    explicit HttpServerContextTestDouble(znet::Address::ptr listen_address)
        : HttpServer(std::move(listen_address)) {}

    using HttpServer::find_websocket_session;
    using HttpServer::is_async_stream_active;
    using HttpServer::is_websocket_active;
    using HttpServer::mark_async_stream_active;
    using HttpServer::mark_async_stream_finished;
    using HttpServer::handle_request;
    using HttpServer::on_message;
    using HttpServer::register_websocket_session;
    using HttpServer::send_async_chunked_response;
    using HttpServer::take_websocket_session;
    using HttpServer::on_close;
    using HttpServer::on_connection;

    znet::TcpServer::ptr tcp_server_for_test() const { return tcp_server(); }
};

TEST(HttpServerContextTest, ParserContextLifecycleFollowsConnection) {
    auto listen_address = std::make_shared<znet::IPv4Address>("127.0.0.1", 0);
    HttpServerContextTestDouble server(listen_address);

    auto socket = znet::Socket::create_tcp();
    ASSERT_TRUE(socket != nullptr);

    auto conn = std::make_shared<znet::TcpConnection>(socket);
    ASSERT_TRUE(conn != nullptr);
    EXPECT_EQ(conn->context(), nullptr);

    server.on_connection(conn);
    EXPECT_NE(conn->context(), nullptr);

    server.on_close(conn);
    EXPECT_EQ(conn->context(), nullptr);
}

TEST(HttpServerContextTest, HttpServerRejectsInvalidTlsCertificate) {
    auto listen_address = std::make_shared<znet::IPv4Address>("127.0.0.1", 0);
    HttpServerContextTestDouble server(listen_address);

    EXPECT_FALSE(server.set_ssl_certificate("/tmp/not-exist-cert.pem",
                                            "/tmp/not-exist-key.pem"));
}

TEST(HttpServerContextTest, AsyncAndWebSocketStateHelpersCoverEdgeCases) {
    auto listen_address = std::make_shared<znet::IPv4Address>("127.0.0.1", 0);
    HttpServerContextTestDouble server(listen_address);

    EXPECT_FALSE(server.is_async_stream_active(nullptr));
    EXPECT_FALSE(server.is_websocket_active(nullptr));
    EXPECT_EQ(server.find_websocket_session(-1), nullptr);
    EXPECT_EQ(server.take_websocket_session(-1), nullptr);

    server.mark_async_stream_active(-1);
    server.mark_async_stream_finished(-1);
    server.register_websocket_session(-1, nullptr);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto conn = std::make_shared<znet::TcpConnection>(
        std::make_shared<znet::Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);

    server.mark_async_stream_active(conn->fd());
    EXPECT_TRUE(server.is_async_stream_active(conn));
    server.mark_async_stream_finished(conn->fd());
    EXPECT_FALSE(server.is_async_stream_active(conn));

    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    request->set_version(HttpVersion::HTTP_1_1);
    WebSocketSession::ptr session = std::make_shared<WebSocketSession>(
        nullptr, request, WebSocketCallbacks{},
        WebSocketOptions{kDefaultWebSocketMaxMessageSize, {}}, "");
    ASSERT_NE(session, nullptr);

    server.register_websocket_session(conn->fd(), session);
    EXPECT_TRUE(server.is_websocket_active(conn));
    EXPECT_NE(server.find_websocket_session(conn->fd()), nullptr);

    auto taken = server.take_websocket_session(conn->fd());
    EXPECT_NE(taken, nullptr);
    EXPECT_FALSE(server.is_websocket_active(conn));
    EXPECT_EQ(server.take_websocket_session(conn->fd()), nullptr);

    conn->close();
    ::close(pair[1]);
}

TEST(HttpServerContextTest, TimeoutSettersClampAsExpected) {
    auto listen_address = std::make_shared<znet::IPv4Address>("127.0.0.1", 0);
    HttpServerContextTestDouble server(listen_address);

    const uint64_t huge_timeout = std::numeric_limits<uint64_t>::max();
    server.set_recv_timeout(huge_timeout);
    server.set_write_timeout(huge_timeout);
    server.set_keepalive_timeout(123456);

    auto tcp = server.tcp_server_for_test();
    ASSERT_NE(tcp, nullptr);
    EXPECT_EQ(tcp->read_timeout(),
              std::numeric_limits<uint32_t>::max() - static_cast<uint32_t>(1));
    EXPECT_EQ(tcp->write_timeout(),
              std::numeric_limits<uint32_t>::max() - static_cast<uint32_t>(1));
    EXPECT_EQ(tcp->keepalive_timeout(), 123456u);
}

TEST(HttpServerContextTest, NullConnectionCallbacksAreNoops) {
    auto listen_address = std::make_shared<znet::IPv4Address>("127.0.0.1", 0);
    HttpServerContextTestDouble server(listen_address);

    server.on_connection(nullptr);
    server.on_close(nullptr);
}

TEST(HttpServerContextTest, OnMessageParseErrorAndWebSocketSessionFailurePaths) {
    auto listen_address = std::make_shared<znet::IPv4Address>("127.0.0.1", 0);
    HttpServerContextTestDouble server(listen_address);

    int pair1[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair1), 0);
    auto conn1 = std::make_shared<znet::TcpConnection>(
        std::make_shared<znet::Socket>(pair1[0]));
    ASSERT_NE(conn1, nullptr);
    server.on_connection(conn1);

    // Host 行缺少 ':'，触发 HTTP 解析错误分支。
    znet::Buffer malformed;
    malformed.append("GET /bad HTTP/1.1\r\n");
    malformed.append("Host localhost\r\n");
    malformed.append("\r\n");
    server.on_message(conn1, malformed);

    int pair2[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair2), 0);
    auto conn2 = std::make_shared<znet::TcpConnection>(
        std::make_shared<znet::Socket>(pair2[0]));
    ASSERT_NE(conn2, nullptr);
    server.on_connection(conn2);

    auto request = std::make_shared<HttpRequest>();
    request->set_method(HttpMethod::GET);
    request->set_version(HttpVersion::HTTP_1_1);
    auto session = std::make_shared<WebSocketSession>(
        nullptr, request, WebSocketCallbacks{},
        WebSocketOptions{kDefaultWebSocketMaxMessageSize, {}}, "");
    server.register_websocket_session(conn2->fd(), session);
    ASSERT_TRUE(server.is_websocket_active(conn2));

    // 非掩码客户端帧会使 session->on_message 返回 false，从会话表移除。
    znet::Buffer invalid_ws_frame;
    invalid_ws_frame.append(std::string("\x81\x01x", 3));
    server.on_message(conn2, invalid_ws_frame);
    EXPECT_FALSE(server.is_websocket_active(conn2));

    conn1->close();
    conn2->close();
    ::close(pair1[1]);
    ::close(pair2[1]);
}

TEST(HttpServerContextTest, HandleRequestCoversSendFailureBranches) {
    auto listen_address = std::make_shared<znet::IPv4Address>("127.0.0.1", 0);
    HttpServerContextTestDouble server(listen_address);
    server.router().get("/plain", [](const HttpRequest::ptr &, HttpResponse &resp) {
        resp.status(HttpStatus::OK).text("ok");
    });
    server.router().get("/chunked", [](const HttpRequest::ptr &, HttpResponse &resp) {
        resp.status(HttpStatus::OK).body("chunk").enable_chunked();
    });

    zco::init(1);
    ::signal(SIGPIPE, SIG_IGN);

    {
        int pair[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
        auto conn = std::make_shared<znet::TcpConnection>(
            std::make_shared<znet::Socket>(pair[0]));
        ASSERT_NE(conn, nullptr);

        server.on_connection(conn);

        auto request_plain = std::make_shared<HttpRequest>();
        request_plain->set_method(HttpMethod::GET);
        request_plain->set_version(HttpVersion::HTTP_1_1);
        request_plain->set_path("/plain");

        auto request_chunked = std::make_shared<HttpRequest>();
        request_chunked->set_method(HttpMethod::GET);
        request_chunked->set_version(HttpVersion::HTTP_1_1);
        request_chunked->set_path("/chunked");

        ::close(pair[1]); // 让写回失败，覆盖 send 错误分支。
        EXPECT_FALSE(server.handle_request(conn, request_plain));
        EXPECT_FALSE(server.handle_request(conn, request_chunked));
        conn->close();
    }

    zco::shutdown();
}

TEST(HttpServerContextTest, HandleRequestWebSocketUpgradeBranches) {
    auto listen_address = std::make_shared<znet::IPv4Address>("127.0.0.1", 0);
    HttpServerContextTestDouble server(listen_address);

    server.router().get("/ws-ok", [](const HttpRequest::ptr &, HttpResponse &resp) {
        resp.upgrade_to_websocket(WebSocketCallbacks{});
    });
    server.router().get("/ws-subproto",
                        [](const HttpRequest::ptr &, HttpResponse &resp) {
                            WebSocketOptions opt;
                            opt.subprotocols = {"chat"};
                            resp.upgrade_to_websocket(WebSocketCallbacks{}, opt);
                        });
    server.router().get("/ws-throw", [](const HttpRequest::ptr &, HttpResponse &resp) {
        WebSocketCallbacks callbacks;
        callbacks.on_open = [](const WebSocketConnection::ptr &,
                               const HttpRequest::ptr &) {
            throw std::runtime_error("open failed");
        };
        resp.upgrade_to_websocket(callbacks);
    });

    auto make_conn = []() {
        int pair[2] = {-1, -1};
        EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
        auto conn = std::make_shared<znet::TcpConnection>(
            std::make_shared<znet::Socket>(pair[0]));
        return std::tuple<znet::TcpConnection::ptr, int, int>{conn, pair[0], pair[1]};
    };

    auto make_ws_request = [](const std::string &path,
                              const std::string &version_header,
                              const std::string &subprotocol) {
        auto request = std::make_shared<HttpRequest>();
        request->set_method(HttpMethod::GET);
        request->set_version(HttpVersion::HTTP_1_1);
        request->set_path(path);
        request->set_header("Connection", "keep-alive, Upgrade");
        request->set_header("Upgrade", "websocket");
        request->set_header("Sec-WebSocket-Version", version_header);
        request->set_header("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
        if (!subprotocol.empty()) {
            request->set_header("Sec-WebSocket-Protocol", subprotocol);
        }
        return request;
    };

    {
        auto conn_tuple = make_conn();
        auto conn = std::get<0>(conn_tuple);
        const int fd1 = std::get<2>(conn_tuple);
        ASSERT_NE(conn, nullptr);
        auto request = make_ws_request("/ws-ok", "12", "");
        EXPECT_FALSE(server.handle_request(conn, request));
        EXPECT_FALSE(server.is_websocket_active(conn));
        conn->close();
        ::close(fd1);
    }

    {
        auto conn_tuple = make_conn();
        auto conn = std::get<0>(conn_tuple);
        const int fd1 = std::get<2>(conn_tuple);
        ASSERT_NE(conn, nullptr);
        auto request = make_ws_request("/ws-subproto", "13", "json");
        EXPECT_FALSE(server.handle_request(conn, request));
        EXPECT_FALSE(server.is_websocket_active(conn));
        conn->close();
        ::close(fd1);
    }

    {
        auto conn_tuple = make_conn();
        auto conn = std::get<0>(conn_tuple);
        const int fd1 = std::get<2>(conn_tuple);
        ASSERT_NE(conn, nullptr);
        auto request = make_ws_request("/ws-throw", "13", "");
        EXPECT_FALSE(server.handle_request(conn, request));
        EXPECT_FALSE(server.is_websocket_active(conn));
        conn->close();
        ::close(fd1);
    }

    {
        auto conn_tuple = make_conn();
        auto conn = std::get<0>(conn_tuple);
        const int fd1 = std::get<2>(conn_tuple);
        ASSERT_NE(conn, nullptr);
        auto request = make_ws_request("/ws-ok", "13", "");
        const bool upgraded = server.handle_request(conn, request);
        if (upgraded) {
            EXPECT_TRUE(server.is_websocket_active(conn));
            server.on_close(conn);
            EXPECT_FALSE(server.is_websocket_active(conn));
        } else {
            EXPECT_FALSE(server.is_websocket_active(conn));
        }
        conn->close();
        ::close(fd1);
    }
}

TEST(HttpServerContextTest, OnMessageReturnsEarlyWhenAsyncStreamIsActive) {
    auto listen_address = std::make_shared<znet::IPv4Address>("127.0.0.1", 0);
    HttpServerContextTestDouble server(listen_address);

    int pair[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
    auto conn = std::make_shared<znet::TcpConnection>(
        std::make_shared<znet::Socket>(pair[0]));
    ASSERT_NE(conn, nullptr);
    server.on_connection(conn);

    server.mark_async_stream_active(conn->fd());

    znet::Buffer request_buffer;
    request_buffer.append("GET /x HTTP/1.1\r\n");
    request_buffer.append("Host: localhost\r\n");
    request_buffer.append("\r\n");
    const size_t before = request_buffer.readable_bytes();

    server.on_message(conn, request_buffer);
    EXPECT_EQ(request_buffer.readable_bytes(), before);

    server.mark_async_stream_finished(conn->fd());
    server.on_close(conn);
    conn->close();
    ::close(pair[1]);
}

TEST(HttpServerContextTest, SendAsyncChunkedResponseCoversFailureAndClosePaths) {
    auto listen_address = std::make_shared<znet::IPv4Address>("127.0.0.1", 0);
    HttpServerContextTestDouble server(listen_address);
    ::signal(SIGPIPE, SIG_IGN);

    {
        HttpResponse response;
        EXPECT_FALSE(server.send_async_chunked_response(nullptr, response));
    }

    {
        int pair[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
        auto conn = std::make_shared<znet::TcpConnection>(
            std::make_shared<znet::Socket>(pair[0]));
        ASSERT_NE(conn, nullptr);

        HttpResponse response;
        EXPECT_FALSE(server.send_async_chunked_response(conn, response));
        conn->close();
        ::close(pair[1]);
    }

    {
        int pair[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
        auto conn = std::make_shared<znet::TcpConnection>(
            std::make_shared<znet::Socket>(pair[0]));
        ASSERT_NE(conn, nullptr);

        HttpResponse response;
        response.set_version(HttpVersion::HTTP_1_1);
        response.async_stream([](HttpResponse::AsyncChunkSender,
                                 HttpResponse::AsyncStreamCloser close) {
            close();
        });

        ::close(pair[1]); // 响应头发送失败。
        EXPECT_FALSE(server.send_async_chunked_response(conn, response));
        conn->close();
    }

    {
        int pair[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
        auto conn = std::make_shared<znet::TcpConnection>(
            std::make_shared<znet::Socket>(pair[0]));
        ASSERT_NE(conn, nullptr);

        int peer_fd = pair[1];
        HttpResponse response;
        response.set_version(HttpVersion::HTTP_1_1);
        response.async_stream([&peer_fd](HttpResponse::AsyncChunkSender send,
                                         HttpResponse::AsyncStreamCloser close) {
            ::close(peer_fd);
            peer_fd = -1;
            EXPECT_FALSE(send("abc"));
            close();
            close(); // 覆盖 finish_stream 的重复关闭分支。
        });

        EXPECT_TRUE(server.send_async_chunked_response(conn, response));
        EXPECT_FALSE(server.is_async_stream_active(conn));
        conn->close();
    }

    {
        int pair[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
        auto conn = std::make_shared<znet::TcpConnection>(
            std::make_shared<znet::Socket>(pair[0]));
        ASSERT_NE(conn, nullptr);

        HttpResponse response;
        response.set_version(HttpVersion::HTTP_1_1);
        response.async_stream([](HttpResponse::AsyncChunkSender send,
                                 HttpResponse::AsyncStreamCloser close) {
            EXPECT_TRUE(send(""));
            close();
        });

        EXPECT_TRUE(server.send_async_chunked_response(conn, response));
        EXPECT_FALSE(server.is_async_stream_active(conn));
        conn->close();
        ::close(pair[1]);
    }

    {
        int pair[2] = {-1, -1};
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
        auto conn = std::make_shared<znet::TcpConnection>(
            std::make_shared<znet::Socket>(pair[0]));
        ASSERT_NE(conn, nullptr);

        HttpResponse response;
        response.set_version(HttpVersion::HTTP_1_1);
        response.async_stream([](HttpResponse::AsyncChunkSender,
                                 HttpResponse::AsyncStreamCloser) {
            throw std::runtime_error("async boom");
        });

        EXPECT_FALSE(server.send_async_chunked_response(conn, response));
        EXPECT_FALSE(server.is_async_stream_active(conn));
        conn->close();
        ::close(pair[1]);
    }
}

} // namespace
} // namespace zhttp

int main(int argc, char **argv) {
    zhttp::init_logger();
    zco::init(1);
    ::testing::InitGoogleTest(&argc, argv);
    const int rc = RUN_ALL_TESTS();
    zco::shutdown();
    return rc;
}
