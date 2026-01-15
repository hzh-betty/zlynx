#include "logger.h"
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

  logger.logImpl(LogLevel::value::INFO, __FILE__, __LINE__, "test message");
}

TEST_F(LoggerTest, AsyncLoggerLog) {
  // Create async logger with safe mode and 100ms timeout
  AsyncLogger logger("test_async", LogLevel::value::DEBUG, formatter, sinks,
                     AsyncType::ASYNC_SAFE, std::chrono::milliseconds(100));

  EXPECT_CALL(*mockSink, log(_, _)).Times(testing::AtLeast(1));

  logger.logImpl(LogLevel::value::INFO, __FILE__, __LINE__,
                 "async test message");

  std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

TEST_F(LoggerTest, LevelFilter) {
  SyncLogger logger("test_filter", LogLevel::value::WARNING, formatter, sinks);

  EXPECT_CALL(*mockSink, log(_, _)).Times(0);
  logger.logImpl(LogLevel::value::INFO, __FILE__, __LINE__,
                 "should not be logged");

  EXPECT_CALL(*mockSink, log(_, _)).Times(1);
  logger.logImpl(LogLevel::value::ERROR, __FILE__, __LINE__,
                 "should be logged");
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
