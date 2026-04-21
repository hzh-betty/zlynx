#include "zlog/logger.h"
#include <chrono>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thread>

using namespace zlog;
using ::testing::_;

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

    EXPECT_CALL(*mockSink, log(_, _)).Times(testing::AtLeast(1));

    logger.log_impl(LogLevel::value::INFO, __FILE__, __LINE__,
                    "async test message");

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
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
    LocalLoggerBuilder builder;
    Logger::ptr logger = builder.build();
    EXPECT_EQ(logger.get(), static_cast<Logger *>(NULL));
}

TEST_F(LoggerTest, GlobalBuilderReturnsNullWhenNameMissing) {
    GlobalLoggerBuilder builder;
    Logger::ptr logger = builder.build();
    EXPECT_EQ(logger.get(), static_cast<Logger *>(NULL));
}

TEST_F(LoggerTest, GlobalBuilderAsyncBranchWithDefaultFormatterAndSink) {
    GlobalLoggerBuilder builder;
    builder.build_logger_name("root");
    builder.build_logger_type(LoggerType::LOGGER_ASYNC);
    builder.build_wait_time(std::chrono::milliseconds(10));

    Logger::ptr logger = builder.build();
    ASSERT_NE(logger.get(), static_cast<Logger *>(NULL));

    logger->log_impl(LogLevel::value::INFO, __FILE__, __LINE__,
                     "global async branch");
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
}

TEST_F(LoggerTest, LoggerManagerUpsertNullLoggerNoop) {
    LoggerManager::get_instance().upsert_logger("upsert_null", Logger::ptr{});
    EXPECT_EQ(LoggerManager::get_instance().get_logger("upsert_null").get(),
              static_cast<Logger *>(NULL));
}

TEST_F(LoggerTest, LoggerManagerUpsertRootReplacesRootLogger) {
    LocalLoggerBuilder builder;
    builder.build_logger_name("upsert_root_logger");
    builder.build_logger_type(LoggerType::LOGGER_SYNC);
    Logger::ptr logger = builder.build();
    ASSERT_NE(logger.get(), static_cast<Logger *>(NULL));

    LoggerManager::get_instance().upsert_logger("root", logger);
    EXPECT_EQ(LoggerManager::get_instance().root_logger(), logger);
    EXPECT_EQ(LoggerManager::get_instance().get_logger("root"), logger);
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
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
}

TEST_F(LoggerTest, LoggerManagerUpsertNonRootDoesNotReplaceRoot) {
    Logger::ptr old_root = LoggerManager::get_instance().root_logger();
    ASSERT_NE(old_root.get(), static_cast<Logger *>(NULL));

    LocalLoggerBuilder builder;
    builder.build_logger_name("non_root_upsert");
    builder.build_logger_type(LoggerType::LOGGER_SYNC);
    Logger::ptr logger = builder.build();
    ASSERT_NE(logger.get(), static_cast<Logger *>(NULL));

    LoggerManager::get_instance().upsert_logger("non_root_upsert", logger);
    EXPECT_EQ(LoggerManager::get_instance().root_logger(), old_root);
    EXPECT_EQ(LoggerManager::get_instance().get_logger("non_root_upsert"),
              logger);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
