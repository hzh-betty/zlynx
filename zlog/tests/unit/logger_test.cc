#include "zlog/logger.h"
#include "zlog/zlog.h"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thread>

using namespace zlog;
using ::testing::_;

struct NestedFormatValue {
    std::string before;
    std::function<void()> callback;
    std::string after;
};

template <> struct fmt::formatter<NestedFormatValue> {
    constexpr auto parse(fmt::format_parse_context &ctx) -> decltype(ctx.begin()) {
        return ctx.begin();
    }
    auto format(const NestedFormatValue &value, fmt::format_context &ctx) const
        -> decltype(ctx.out()) {
        // 正文已经写入一部分后触发嵌套调用，验证外层格式化缓存不会被覆盖。
        fmt::format_to(ctx.out(), "{}", value.before);
        if (value.callback) value.callback();
        return fmt::format_to(ctx.out(), "{}", value.after);
    }
};

class MockLogSink : public LogSink {
  public:
    MOCK_METHOD(void, log, (const char *data, size_t len), (override));
};

class LoggerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        formatter = std::make_shared<Formatter>();
        mockSink = std::make_shared<MockLogSink>();
        sinks.push_back(mockSink);
    }

    std::shared_ptr<Formatter> formatter;
    std::shared_ptr<MockLogSink> mockSink;
    std::vector<LogSink::ptr> sinks;
};

TEST_F(LoggerTest, SyncLoggerLog) {
    SyncLogger logger("test_sync", LogLevel::value::DEBUG, formatter, sinks);

    EXPECT_CALL(*mockSink, log(_, _)).Times(1);

    logger.log_impl(LogLevel::value::INFO, __FILE__, __LINE__, "test message");
}

TEST_F(LoggerTest, AsyncLoggerLog) {
    // Create async logger with safe mode and 100ms timeout
    AsyncLogger logger("test_async", LogLevel::value::DEBUG, formatter, sinks,
                       AsyncType::ASYNC_SAFE, std::chrono::milliseconds(100));

    EXPECT_CALL(*mockSink, log(_, _)).Times(1);

    logger.log_impl(LogLevel::value::INFO, __FILE__, __LINE__,
                    "async test message");

    logger.flush();
}

TEST_F(LoggerTest, LevelFilter) {
    SyncLogger logger("test_filter", LogLevel::value::WARNING, formatter,
                      sinks);

    EXPECT_CALL(*mockSink, log(_, _)).Times(0);
    logger.log_impl(LogLevel::value::INFO, __FILE__, __LINE__,
                    "should not be logged");

    EXPECT_CALL(*mockSink, log(_, _)).Times(1);
    logger.log_impl(LogLevel::value::ERROR, __FILE__, __LINE__,
                    "should be logged");
}

TEST_F(LoggerTest, LocalBuilderReturnsNullWhenNameMissing) {
    LoggerBuilder builder;
    Logger::ptr logger = builder.build();
    EXPECT_EQ(logger.get(), static_cast<Logger *>(NULL));
}

TEST_F(LoggerTest, BuilderAsyncBranchWithDefaultFormatterAndSink) {
    LoggerBuilder builder;
    builder.build_logger_name("root");
    builder.build_logger_type(LoggerType::LOGGER_ASYNC);
    builder.build_wait_time(std::chrono::milliseconds(10));

    Logger::ptr logger = builder.build();
    ASSERT_NE(logger.get(), static_cast<Logger *>(NULL));

    logger->log_impl(LogLevel::value::INFO, __FILE__, __LINE__,
                     "global async branch");
    logger->flush();
}

TEST_F(LoggerTest, SyncLoggerWithEmptySinksReturnsEarly) {
    std::vector<LogSink::ptr> empty_sinks;
    SyncLogger logger("sync_empty", LogLevel::value::DEBUG, formatter,
                      empty_sinks);
    logger.log_impl(LogLevel::value::INFO, __FILE__, __LINE__,
                    "should be dropped");
}

TEST_F(LoggerTest, AsyncLoggerWithEmptySinksReturnsEarlyInRelog) {
    std::vector<LogSink::ptr> empty_sinks;
    AsyncLogger logger("async_empty", LogLevel::value::DEBUG, formatter,
                       empty_sinks, AsyncType::ASYNC_SAFE,
                       std::chrono::milliseconds(10));
    logger.log_impl(LogLevel::value::INFO, __FILE__, __LINE__, "async dropped");
    logger.flush();
}

TEST_F(LoggerTest, UninitializedLoggerThrowsWhenLogging) {
    EXPECT_THROW(zlog::get_logger("uninitialized_logger")->ZLOG_INFO(
                     "uninitialized logger"),
                 std::runtime_error);
}

TEST_F(LoggerTest, ExplicitRegistrationRejectsDuplicateNames) {
    LoggerBuilder builder;
    builder.build_logger_name("global_duplicate_test");
    const auto first = builder.build();
    ASSERT_NE(first, nullptr);
    LoggerManager::get_instance().add_logger(first);
    const auto second = builder.build();
    EXPECT_THROW(LoggerManager::get_instance().add_logger(second), std::invalid_argument);
    EXPECT_EQ(LoggerManager::get_instance().get_logger("global_duplicate_test"), first);
}

TEST_F(LoggerTest, LocalBuilderDoesNotRegisterLogger) {
    LoggerBuilder builder;
    builder.build_logger_name("local_unregistered_test");
    ASSERT_NE(builder.build(), nullptr);
    EXPECT_FALSE(LoggerManager::get_instance().get_logger("local_unregistered_test"));
}

TEST_F(LoggerTest, RegistrationRejectsNullLogger) {
    EXPECT_THROW(LoggerManager::get_instance().add_logger(Logger::ptr()),
                 std::invalid_argument);
}

TEST_F(LoggerTest, SameBuilderSupportsLocalConstructionThenGlobalRegistration) {
    LoggerBuilder builder;
    builder.build_logger_name("local_then_global_test");
    const auto local = builder.build();
    ASSERT_NE(local, nullptr);
    EXPECT_FALSE(LoggerManager::get_instance().get_logger("local_then_global_test"));

    const auto global = builder.build();
    ASSERT_NE(global, nullptr);
    LoggerManager::get_instance().add_logger(global);
    EXPECT_NE(local, global);
    EXPECT_EQ(LoggerManager::get_instance().get_logger("local_then_global_test"), global);
}

namespace {
class RecordingSink : public LogSink {
  public:
    void log(const char *data, size_t len) override { records.emplace_back(data, len); }
    void flush() override { ++flushes; }
    std::vector<std::string> records;
    int flushes = 0;
};

class CallbackSink : public LogSink {
  public:
    explicit CallbackSink(std::function<void()> callback) : callback_(std::move(callback)) {}
    void log(const char *, size_t) override { callback_(); }
  private:
    std::function<void()> callback_;
};

class FailingSink : public LogSink {
  public:
    explicit FailingSink(bool fail_write = true) : fail_write_(fail_write) {}
    void log(const char *, size_t) override {
        if (fail_write_) throw std::runtime_error("write failed");
    }
    void flush() override {
        if (!fail_write_) throw std::runtime_error("flush failed");
    }
  private:
    bool fail_write_;
};
} // namespace

TEST_F(LoggerTest, NestedLoggingKeepsOuterBufferAliveAcrossExpansion) {
    const auto pattern = std::make_shared<Formatter>("%m");
    const auto inner_sink = std::make_shared<RecordingSink>();
    const auto outer_sink = std::make_shared<RecordingSink>();
    SyncLogger inner("inner", LogLevel::value::INFO, pattern, {inner_sink});
    const auto callback = std::make_shared<CallbackSink>([&] {
        inner.info(__FILE__, __LINE__, "{}", std::string(4096, 'i'));
    });
    SyncLogger outer("outer", LogLevel::value::INFO, pattern, {callback, outer_sink});
    // 内层的大消息不能使外层正文失效；该路径同时用于 ASan 回归。
    outer.info(__FILE__, __LINE__, "{}", std::string(1024, 'o'));
    ASSERT_EQ(outer_sink->records.size(), 1u);
    EXPECT_EQ(outer_sink->records.front(), std::string(1024, 'o'));
    EXPECT_EQ(inner_sink->records.front(), std::string(4096, 'i'));
}

TEST_F(LoggerTest, NestedParameterFormattingPreservesPartialPayloadAndRecoversAfterException) {
    const auto pattern = std::make_shared<Formatter>("%m");
    const auto inner_sink = std::make_shared<RecordingSink>();
    const auto outer_sink = std::make_shared<RecordingSink>();
    SyncLogger inner("inner_format", LogLevel::value::INFO, pattern, {inner_sink});
    SyncLogger outer("outer_format", LogLevel::value::INFO, pattern, {outer_sink});
    const NestedFormatValue inner_value{std::string(4096, 'i'), {}, "-inner"};
    const NestedFormatValue outer_value{std::string(1024, 'o'), [&] {
        inner.info(__FILE__, __LINE__, "{}", inner_value);
    }, "-outer"};
    const NestedFormatValue failed_value{std::string(2048, 'f'), [] {
        throw std::runtime_error("expected format failure");
    }, "-failed"};

    // 两次正常调用之间插入格式化异常，缓存仍能继续使用，失败正文不能混入后续记录。
    outer.info(__FILE__, __LINE__, "{}", outer_value);
    EXPECT_THROW(outer.info(__FILE__, __LINE__, "{}", failed_value), std::runtime_error);
    outer.info(__FILE__, __LINE__, "{}", outer_value);
    EXPECT_EQ(outer_sink->records, (std::vector<std::string>(2, std::string(1024, 'o') + "-outer")));
    EXPECT_EQ(inner_sink->records, (std::vector<std::string>(2, std::string(4096, 'i') + "-inner")));
}

TEST_F(LoggerTest, MultipleNestedSinkCallsPreserveEveryActiveSerializedBuffer) {
    const auto pattern = std::make_shared<Formatter>("%m");
    const auto inner_sink = std::make_shared<RecordingSink>();
    const auto middle_sink = std::make_shared<RecordingSink>();
    const auto outer_sink = std::make_shared<RecordingSink>();
    SyncLogger inner("deep_inner", LogLevel::value::INFO, pattern, {inner_sink});
    const auto middle_callback = std::make_shared<CallbackSink>([&] {
        inner.info(__FILE__, __LINE__, "{}", std::string(8192, 'i'));
    });
    SyncLogger middle("deep_middle", LogLevel::value::INFO, pattern, {middle_callback, middle_sink});
    const auto outer_callback = std::make_shared<CallbackSink>([&] {
        middle.info(__FILE__, __LINE__, "{}", std::string(4096, 'm'));
    });
    SyncLogger outer("deep_outer", LogLevel::value::INFO, pattern, {outer_callback, outer_sink});
    outer.info(__FILE__, __LINE__, "{}", std::string(1024, 'o'));
    EXPECT_EQ(outer_sink->records, (std::vector<std::string>{std::string(1024, 'o')}));
    EXPECT_EQ(middle_sink->records, (std::vector<std::string>{std::string(4096, 'm')}));
    EXPECT_EQ(inner_sink->records, (std::vector<std::string>{std::string(8192, 'i')}));
}

TEST_F(LoggerTest, SyncFailureStillWritesAndFlushesOtherSinks) {
    const auto healthy = std::make_shared<RecordingSink>();
    SyncLogger logger("failure", LogLevel::value::INFO, std::make_shared<Formatter>("%m"),
                      {std::make_shared<FailingSink>(), healthy});
    EXPECT_THROW(logger.info(__FILE__, __LINE__, "hello"), std::runtime_error);
    ASSERT_EQ(healthy->records.size(), 1u);
    EXPECT_EQ(healthy->records.front(), "hello");
    logger.close();
    EXPECT_EQ(healthy->flushes, 1);
    EXPECT_NO_THROW(logger.close());
    EXPECT_THROW(logger.info(__FILE__, __LINE__, "after close"), std::runtime_error);
}

TEST_F(LoggerTest, AsyncFailureReportsThroughFlushAndCloseWithoutDroppingOtherRecords) {
    const auto healthy = std::make_shared<RecordingSink>();
    AsyncLogger logger("async_failure", LogLevel::value::INFO,
                       std::make_shared<Formatter>("%m"),
                       {std::make_shared<FailingSink>(), healthy},
                       AsyncType::ASYNC_SAFE, std::chrono::hours(1));
    for (int i = 0; i < 5; ++i) logger.info(__FILE__, __LINE__, "message {}", i);
    EXPECT_THROW(logger.flush(), std::runtime_error);
    ASSERT_EQ(healthy->records.size(), 5u);
    for (int i = 0; i < 5; ++i) EXPECT_EQ(healthy->records[i], "message " + std::to_string(i));
    EXPECT_EQ(healthy->flushes, 1);
    EXPECT_THROW(logger.close(), std::runtime_error);
    EXPECT_NO_THROW(logger.close());
    EXPECT_THROW(logger.info(__FILE__, __LINE__, "after close"), std::runtime_error);
}

TEST_F(LoggerTest, FlushFailuresStillFlushOtherSinksAndCloseReleasesOwnership) {
    const auto healthy = std::make_shared<RecordingSink>();
    for (bool async : {false, true}) {
        const auto failed = std::make_shared<FailingSink>(false);
        Logger::ptr logger;
        const std::vector<LogSink::ptr> targets{failed, healthy};
        if (async) {
            logger = std::make_shared<AsyncLogger>("flush_failure", LogLevel::value::INFO,
                formatter, targets, AsyncType::ASYNC_SAFE, std::chrono::hours(1));
        } else {
            logger = std::make_shared<SyncLogger>("flush_failure", LogLevel::value::INFO,
                                                formatter, targets);
        }
        const int before = healthy->flushes;
        EXPECT_THROW(logger->flush(), std::runtime_error);
        EXPECT_EQ(healthy->flushes, before + 1);
        EXPECT_THROW(logger->close(), std::runtime_error);
        EXPECT_EQ(healthy->flushes, before + 2);
        EXPECT_NO_THROW(logger->close());
    }
}

TEST_F(LoggerTest, FlushWaitsForSinkWriteAndFlushCompletion) {
    std::mutex gate;
    std::condition_variable cv;
    bool entered = false, release = false;
    const auto blocking = std::make_shared<CallbackSink>([&] {
        std::unique_lock<std::mutex> lock(gate);
        entered = true;
        cv.notify_all();
        cv.wait(lock, [&] { return release; });
    });
    const auto healthy = std::make_shared<RecordingSink>();
    AsyncLogger logger("barrier", LogLevel::value::INFO, formatter, {blocking, healthy},
                       AsyncType::ASYNC_SAFE, std::chrono::hours(1));
    logger.info(__FILE__, __LINE__, "pending");
    auto completion = std::async(std::launch::async, [&] { logger.flush(); });
    {
        std::unique_lock<std::mutex> lock(gate);
        cv.wait(lock, [&] { return entered; });
    }
    EXPECT_EQ(completion.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);
    {
        std::lock_guard<std::mutex> lock(gate);
        release = true;
    }
    cv.notify_all();
    completion.get();
    EXPECT_EQ(healthy->records.size(), 1u);
    EXPECT_EQ(healthy->flushes, 1);
}

TEST_F(LoggerTest, AsyncFlushPreservesRecordBoundariesIncludingEmptyAndBinaryMessages) {
    const auto sink = std::make_shared<RecordingSink>();
    AsyncLogger logger("records", LogLevel::value::INFO, std::make_shared<Formatter>("%m"),
                       {sink}, AsyncType::ASYNC_SAFE, std::chrono::hours(1));
    logger.info(__FILE__, __LINE__, "first");
    logger.info(__FILE__, __LINE__, "");
    logger.info(__FILE__, __LINE__, "{}", std::string("a\0b", 3));
    logger.flush();
    EXPECT_EQ(sink->records, (std::vector<std::string>{"first", "", std::string("a\0b", 3)}));
    logger.close();
    EXPECT_NO_THROW(logger.flush());
}

TEST_F(LoggerTest, SinkReentryIntoItsOwnLoggerReportsErrorInsteadOfDeadlocking) {
    Logger::ptr logger;
    const auto callback = std::make_shared<CallbackSink>([&] { logger->flush(); });
    logger = std::make_shared<SyncLogger>("reentrant", LogLevel::value::INFO, formatter,
                                         std::vector<LogSink::ptr>{callback});
    EXPECT_THROW(logger->info(__FILE__, __LINE__, "hello"), std::logic_error);
    logger->close();
    logger = std::make_shared<AsyncLogger>("async_reentrant", LogLevel::value::INFO, formatter,
        std::vector<LogSink::ptr>{callback}, AsyncType::ASYNC_SAFE, std::chrono::hours(1));
    logger->info(__FILE__, __LINE__, "hello");
    EXPECT_THROW(logger->flush(), std::logic_error);
    EXPECT_THROW(logger->close(), std::logic_error);
}

TEST_F(LoggerTest, CloseReleasesUnsharedSinksWhileLoggerRemainsAlive) {
    for (bool async : {false, true}) {
        std::weak_ptr<LogSink> lifetime;
        Logger::ptr logger;
        {
            const auto sink = std::make_shared<RecordingSink>();
            lifetime = sink;
            const std::vector<LogSink::ptr> targets{sink};
            if (async) {
                logger = std::make_shared<AsyncLogger>("close_owner", LogLevel::value::INFO,
                    formatter, targets, AsyncType::ASYNC_SAFE, std::chrono::hours(1));
            } else {
                logger = std::make_shared<SyncLogger>("close_owner", LogLevel::value::INFO,
                                                    formatter, targets);
            }
        }
        EXPECT_FALSE(lifetime.expired());
        logger->close();
        EXPECT_TRUE(lifetime.expired());
        EXPECT_NO_THROW(logger->close());
    }
}

TEST_F(LoggerTest, ConcurrentCloseDrainsEveryAcceptedRecordAndRejectsNewWrites) {
    const auto sink = std::make_shared<RecordingSink>();
    AsyncLogger logger("closing", LogLevel::value::INFO, std::make_shared<Formatter>("%m"),
                       {sink}, AsyncType::ASYNC_SAFE, std::chrono::hours(1));
    std::atomic<int> accepted{0};
    std::promise<void> started;
    const auto start = started.get_future();
    std::thread writer([&] {
        for (int i = 0; i < 10000; ++i) {
            try {
                logger.info(__FILE__, __LINE__, "{}", i);
                ++accepted;
                if (i == 0) started.set_value();
            } catch (const std::runtime_error &) {
                break;
            }
        }
    });
    start.wait();
    std::thread closer([&] { logger.close(); });
    logger.close();
    writer.join();
    closer.join();
    EXPECT_EQ(sink->records.size(), static_cast<size_t>(accepted.load()));
    for (int i = 0; i < accepted; ++i) EXPECT_EQ(sink->records[i], std::to_string(i));
    EXPECT_EQ(sink->flushes, 1);
    EXPECT_THROW(logger.info(__FILE__, __LINE__, "new"), std::runtime_error);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
