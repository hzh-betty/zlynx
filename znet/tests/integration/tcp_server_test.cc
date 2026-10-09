#include "../support/network_fixture.h"
#include "zco/sync/event.h"
#include "znet/server/tcp_server.h"
#include <gtest/gtest.h>
#include <thread>

using namespace znet;
using namespace std::chrono_literals;

namespace {
Endpoint loopback() { return Endpoint::ipv4("127.0.0.1", 0).value(); }

SessionFactory discard() {
    return [](const Connection::ptr &) { return SessionCallbacks{}; };
}
} // namespace

TEST(TcpServerTest,
     StackOwnershipRestartAndSharedRuntimeHaveIndependentLifetimes) {
    zco::Runtime runtime(zco::RuntimeOptions{2});
    TcpServer first(runtime, loopback(), discard());
    TcpServer second(runtime, loopback(), discard());
    EXPECT_FALSE(first.is_running());
    ASSERT_TRUE(first.start());
    ASSERT_TRUE(first.start());
    ASSERT_TRUE(second.start());
    EXPECT_GT(first.local_endpoint().value().port(), 0);
    first.stop();
    first.stop();
    EXPECT_FALSE(first.is_running());
    EXPECT_TRUE(second.is_running());
    ASSERT_TRUE(first.start());
    EXPECT_TRUE(first.is_running());
    first.stop();
    auto task = runtime.spawn([] {});
    ASSERT_TRUE(task);
    EXPECT_TRUE(task.value().join());
    second.stop();
}

TEST(TcpServerTest, SessionsOwnProtocolStateAndStopWaitsForCloseCallbacks) {
    zco::Runtime runtime(zco::RuntimeOptions{2});
    zco::Event opened(true);
    std::atomic<int> closed{0}, destroyed{0};

    struct ProtocolState {
        explicit ProtocolState(std::atomic<int> &count) : count(count) {}

        ~ProtocolState() { ++count; }

        std::atomic<int> &count;
    };

    TcpServer server(runtime, loopback(), [&](const Connection::ptr &) {
        auto state = std::make_shared<ProtocolState>(destroyed);
        opened.signal();
        return SessionCallbacks{
            [state](const Connection::ptr &connection, ByteBuffer &input) {
                EXPECT_TRUE(connection->send(input.view()));
                input.retrieve_all();
            },
            [state, &closed](const Connection::ptr &connection) {
                EXPECT_EQ(connection->state(), Connection::State::closed);
                ++closed;
            }};
    });
    ASSERT_TRUE(server.start());
    auto client = test::connect_to(server.local_endpoint().value());
    ASSERT_TRUE(opened.wait(zco::Deadline::after(1s)));
    ASSERT_EQ(::send(client.native_handle(), "echo", 4, MSG_NOSIGNAL), 4);
    EXPECT_EQ(test::receive(client.native_handle(), 4), "echo");
    const auto started = std::chrono::steady_clock::now();
    server.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);
    EXPECT_EQ(closed, 1);
    EXPECT_EQ(destroyed, 1);
    EXPECT_EQ(server.active_connections(), 0u);
    EXPECT_FALSE(server.is_running());
}

TEST(TcpServerTest, RequestStopFromCallbackDoesNotJoinItself) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    std::atomic<int> closed{0};
    zco::Event requested(true);
    TcpServer *active = nullptr;
    TcpServer server(runtime, loopback(), [&](const Connection::ptr &) {
        EXPECT_THROW(active->stop(), std::logic_error);
        active->request_stop();
        requested.signal();
        return SessionCallbacks{{}, [&](const Connection::ptr &) { ++closed; }};
    });
    active = &server;
    ASSERT_TRUE(server.start());
    auto client = test::connect_to(server.local_endpoint().value());
    ASSERT_TRUE(requested.wait(zco::Deadline::after(1s)));
    server.stop();
    EXPECT_EQ(closed, 1);
    EXPECT_EQ(server.active_connections(), 0u);
}

TEST(TcpServerTest, DestructionCancelsIdleConnectionsWithoutStoppingRuntime) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    std::atomic<int> closed{0};
    zco::Event opened(true);
    std::optional<Socket> client;
    {
        TcpServer server(runtime, loopback(), [&](const Connection::ptr &) {
            opened.signal();
            return SessionCallbacks{{},
                                    [&](const Connection::ptr &) { ++closed; }};
        });
        ASSERT_TRUE(server.start());
        client.emplace(test::connect_to(server.local_endpoint().value()));
        ASSERT_TRUE(opened.wait(zco::Deadline::after(1s)));
    }
    EXPECT_EQ(closed, 1);
    EXPECT_TRUE(runtime.executor(0).valid());
}

TEST(TcpServerTest, DestructionInsideCallbackLeavesNoFacadeCapture) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    zco::Event destroyed(true);
    std::unique_ptr<TcpServer> server;
    server = std::make_unique<TcpServer>(runtime, loopback(),
                                         [&](const Connection::ptr &) {
                                             server.reset();
                                             destroyed.signal();
                                             return SessionCallbacks{};
                                         });
    ASSERT_TRUE(server->start());
    auto client = test::connect_to(server->local_endpoint().value());
    ASSERT_TRUE(destroyed.wait(zco::Deadline::after(1s)));
    runtime.request_stop();
    runtime.join();
    EXPECT_EQ(server, nullptr);
}

TEST(TcpServerTest, CallbackExceptionsAreReportedOnceAndStillCleanUp) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    zco::Event closed(true);
    std::atomic<int> errors{0};
    ServerOptions options;
    options.on_error = [&](const Error &error) {
        EXPECT_EQ(error.kind, ErrorKind::application);
        EXPECT_NE(error.detail.find("business failed"), std::string::npos);
        ++errors;
    };
    TcpServer server(
        runtime, loopback(),
        [&](const Connection::ptr &) {
            return SessionCallbacks{
                [](const Connection::ptr &, ByteBuffer &) {
                    throw std::runtime_error("business failed");
                },
                [&](const Connection::ptr &) { closed.signal(); }};
        },
        options);
    ASSERT_TRUE(server.start());
    auto client = test::connect_to(server.local_endpoint().value());
    ASSERT_EQ(::send(client.native_handle(), "x", 1, MSG_NOSIGNAL), 1);
    ASSERT_TRUE(closed.wait(zco::Deadline::after(1s)));
    server.stop();
    EXPECT_EQ(errors, 1);
    EXPECT_EQ(server.active_connections(), 0u);
}

TEST(TcpServerTest, IdleTimeoutWorksWithoutPollingReadTimeout) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    zco::Event closed(true);
    ServerOptions options;
    options.idle_timeout = 30ms;
    TcpServer server(
        runtime, loopback(),
        [&](const Connection::ptr &) {
            return SessionCallbacks{
                {}, [&](const Connection::ptr &) { closed.signal(); }};
        },
        options);
    ASSERT_TRUE(server.start());
    auto client = test::connect_to(server.local_endpoint().value());
    EXPECT_TRUE(closed.wait(zco::Deadline::after(500ms)));
    server.stop();
}

TEST(TcpServerTest, FailedBindAndExpiredRuntimeRollBackStart) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    TcpServer first(runtime, loopback(), discard());
    ASSERT_TRUE(first.start());
    TcpServer occupied(runtime, first.local_endpoint().value(), discard());
    auto failure = occupied.start();
    ASSERT_FALSE(failure);
    EXPECT_EQ(failure.error().code, std::errc::address_in_use);
    EXPECT_FALSE(occupied.is_running());
    first.stop();
    runtime.request_stop();
    auto canceled = first.start();
    ASSERT_FALSE(canceled);
    EXPECT_EQ(canceled.error().code, std::errc::operation_canceled);
    EXPECT_FALSE(first.is_running());
}

TEST(TcpServerTest,
     ConfigurationIsValidatedAndConcurrentControlCallsSerialize) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    EXPECT_THROW(TcpServer(runtime, loopback(), {}), std::invalid_argument);
    ServerOptions invalid;
    invalid.read_chunk_size = 0;
    EXPECT_THROW(TcpServer(runtime, loopback(), discard(), invalid),
                 std::invalid_argument);
    TcpServer server(runtime, loopback(), discard());
    std::vector<std::thread> controllers;
    for (int i = 0; i < 4; ++i)
        controllers.emplace_back([&] {
            for (int round = 0; round < 10; ++round) {
                EXPECT_TRUE(server.start());
                server.stop();
            }
        });
    for (auto &controller : controllers)
        controller.join();
    EXPECT_FALSE(server.is_running());
}
