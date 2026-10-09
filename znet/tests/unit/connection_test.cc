#include "znet/server/connection.h"
#include <cstring>
#include <gtest/gtest.h>

using namespace znet;
using namespace std::chrono_literals;

namespace {
struct Script {
    std::function<Transfer(void *, size_t, zco::Deadline)> read;
    std::function<Transfer(const void *, size_t, zco::Deadline)> write;
    std::function<Result<void>(zco::Deadline)> start;
    std::atomic<int> closed{0};
};

class ScriptedStream final : public ByteStream {
  public:
    explicit ScriptedStream(std::shared_ptr<Script> script)
        : script_(std::move(script)) {}

    Result<void> start(zco::Deadline deadline) override {
        return script_->start ? script_->start(deadline) : Result<void>{};
    }

    Transfer read_some(void *bytes, size_t size,
                       zco::Deadline deadline) override {
        return script_->read(bytes, size, deadline);
    }

    Transfer write_some(const void *bytes, size_t size,
                        zco::Deadline deadline) override {
        return script_->write(bytes, size, deadline);
    }

    Result<void> shutdown_write(zco::Deadline) override { return {}; }

    Result<void> close() override {
        ++script_->closed;
        return {};
    }

    Result<Endpoint> local_endpoint() const override {
        return Endpoint::ipv4("127.0.0.1", 1);
    }

    Result<Endpoint> remote_endpoint() const override {
        return Endpoint::ipv4("127.0.0.1", 2);
    }

    int native_handle() const override { return -1; }

  private:
    std::shared_ptr<Script> script_;
};
} // namespace

TEST(ConnectionTest, AggregatesPartialWritesUnderOneDeadline) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    auto script = std::make_shared<Script>();
    std::string written;
    std::vector<zco::Deadline::TimePoint> deadlines;
    script->write = [&](const void *bytes, size_t size,
                        zco::Deadline deadline) {
        deadlines.push_back(deadline.time());
        const size_t count = std::min(size, size_t{2});
        written.append(static_cast<const char *>(bytes), count);
        return Transfer{count, {}, false};
    };
    Connection connection(std::make_unique<ScriptedStream>(script),
                          runtime.executor(0));
    ASSERT_TRUE(connection.start());
    auto result = connection.send("abcdef", 100ms);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.bytes, 6u);
    EXPECT_EQ(written, "abcdef");
    ASSERT_EQ(deadlines.size(), 3u);
    EXPECT_EQ(deadlines[0], deadlines[1]);
    EXPECT_EQ(deadlines[1], deadlines[2]);
}

TEST(ConnectionTest, PreservesPartialProgressAndClosesAfterWriteFailure) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    auto script = std::make_shared<Script>();
    script->write = [](const void *, size_t, zco::Deadline) {
        return Transfer{2, io_error("write", EPIPE), false};
    };
    Connection connection(std::make_unique<ScriptedStream>(script),
                          runtime.executor(0));
    ASSERT_TRUE(connection.start());
    auto result = connection.send("abcdef");
    EXPECT_FALSE(result);
    EXPECT_EQ(result.bytes, 2u);
    EXPECT_EQ(result.error.code, std::errc::broken_pipe);
    EXPECT_EQ(connection.state(), Connection::State::closed);
    EXPECT_GT(script->closed, 0);
}

TEST(ConnectionTest, ZeroProgressDoesNotLoopOrReportSuccess) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    auto script = std::make_shared<Script>();
    script->write = [](const void *, size_t, zco::Deadline) {
        return Transfer{};
    };
    Connection connection(std::make_unique<ScriptedStream>(script),
                          runtime.executor(0));
    ASSERT_TRUE(connection.start());
    EXPECT_FALSE(connection.send("x"));
    EXPECT_FALSE(connection.connected());
}

TEST(ConnectionTest, ReadAppendsProgressAndUsesEarlierApplicationDeadline) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    auto script = std::make_shared<Script>();
    const auto deadline = zco::Deadline::after(1s);
    script->read = [&](void *bytes, size_t size, zco::Deadline received) {
        EXPECT_GE(size, 2u);
        EXPECT_EQ(received.time(), deadline.time());
        std::memcpy(bytes, "xy", 2);
        return Transfer{2, {}, false};
    };
    Connection connection(std::make_unique<ScriptedStream>(script),
                          runtime.executor(0));
    ASSERT_TRUE(connection.start());
    connection.set_read_deadline(deadline);
    ByteBuffer input(1);
    input.append("prefix:");
    auto result = connection.read(input, 2, zco::Deadline::after(2s));
    EXPECT_TRUE(result);
    EXPECT_EQ(input.view(), "prefix:xy");
    EXPECT_FALSE(connection.read(input, 0));
}

TEST(ConnectionTest, ReadExceptionReleasesGateAndEofStillAllowsWrites) {
    zco::Runtime runtime(zco::RuntimeOptions{1});
    auto script = std::make_shared<Script>();
    int attempts = 0;
    script->read = [&](void *, size_t, zco::Deadline) -> Transfer {
        if (!attempts++)
            throw std::runtime_error("read failed");
        return {0, {}, true};
    };
    script->write = [](const void *, size_t size, zco::Deadline) {
        return Transfer{size, {}, false};
    };
    Connection connection(std::make_unique<ScriptedStream>(script),
                          runtime.executor(0));
    ASSERT_TRUE(connection.start());
    ByteBuffer input;
    EXPECT_THROW((void)connection.read(input), std::runtime_error);
    auto read = connection.read(input);
    EXPECT_TRUE(read.eof);
    EXPECT_TRUE(input.view().empty());
    EXPECT_EQ(connection.state(), Connection::State::peer_closed);
    EXPECT_TRUE(connection.send("response"));
    EXPECT_TRUE(connection.close());
    EXPECT_EQ(connection.state(), Connection::State::closed);
}

TEST(ConnectionTest, StartupFailureAndExpiredExecutorAreExplicit) {
    auto script = std::make_shared<Script>();
    script->start = [](zco::Deadline) -> Result<void> {
        return make_error(ErrorKind::protocol, std::errc::protocol_error,
                          "handshake");
    };
    zco::Runtime runtime(zco::RuntimeOptions{1});
    Connection failed(std::make_unique<ScriptedStream>(script),
                      runtime.executor(0));
    auto result = failed.start();
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, ErrorKind::protocol);
    EXPECT_EQ(failed.state(), Connection::State::closed);
    Connection expired(std::make_unique<ScriptedStream>(script), {});
    auto rejected = expired.start();
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, std::errc::operation_canceled);
    EXPECT_THROW(Connection(nullptr, {}), std::invalid_argument);
}
