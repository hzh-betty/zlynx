#include "format.h"
#include "message.h"
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <regex>

using namespace zlog;

class FormatTest : public ::testing::Test {
protected:
  void SetUp() override {
    // 创建基础日志消息用于测试
    msg.reset(new LogMessage(LogLevel::value::INFO, "test_file.cc", 42,
                             "test message", "test_logger"));
  }

  std::shared_ptr<LogMessage> msg;
};

// ===================== FormatItem Tests =====================

TEST_F(FormatTest, MessageFormatItem) {
  MessageFormatItem item;
  fmt::memory_buffer buffer;
  item.format(buffer, *msg);

  std::string result(buffer.data(), buffer.size());
  EXPECT_EQ(result, "test message");
}

TEST_F(FormatTest, MessageFormatItemEmpty) {
  LogMessage emptyMsg(LogLevel::value::INFO, "test.cc", 1, "", "logger");
  MessageFormatItem item;
  fmt::memory_buffer buffer;
  item.format(buffer, emptyMsg);

  std::string result(buffer.data(), buffer.size());
  EXPECT_EQ(result, "");
}

TEST_F(FormatTest, LevelFormatItem) {
  LevelFormatItem item;
  fmt::memory_buffer buffer;
  item.format(buffer, *msg);

  std::string result(buffer.data(), buffer.size());
  EXPECT_EQ(result, "INFO");
}

TEST_F(FormatTest, LevelFormatItemAllLevels) {
  LevelFormatItem item;

  std::vector<std::pair<LogLevel::value, std::string>> levels;
  levels.push_back(
      std::make_pair(LogLevel::value::DEBUG, std::string("DEBUG")));
  levels.push_back(std::make_pair(LogLevel::value::INFO, std::string("INFO")));
  levels.push_back(
      std::make_pair(LogLevel::value::WARNING, std::string("WARNING")));
  levels.push_back(
      std::make_pair(LogLevel::value::ERROR, std::string("ERROR")));
  levels.push_back(
      std::make_pair(LogLevel::value::FATAL, std::string("FATAL")));

  for (size_t i = 0; i < levels.size(); ++i) {
    LogLevel::value level = levels[i].first;
    const std::string &expected = levels[i].second;
    LogMessage testMsg(level, "test.cc", 1, "msg", "logger");
    fmt::memory_buffer buffer;
    item.format(buffer, testMsg);
    std::string result(buffer.data(), buffer.size());
    EXPECT_EQ(result, expected) << "Failed for level: " << expected;
  }
}

TEST_F(FormatTest, TimeFormatItemDefault) {
  TimeFormatItem item;
  fmt::memory_buffer buffer;
  item.format(buffer, *msg);

  std::string result(buffer.data(), buffer.size());
  // 默认格式 %H:%M:%S，应匹配 HH:MM:SS 格式
  std::regex timePattern(R"(\d{2}:\d{2}:\d{2})");
  EXPECT_TRUE(std::regex_match(result, timePattern)) << "Got: " << result;
}

TEST_F(FormatTest, TimeFormatItemCustom) {
  TimeFormatItem item("%Y-%m-%d");
  fmt::memory_buffer buffer;
  item.format(buffer, *msg);

  std::string result(buffer.data(), buffer.size());
  // 时间格式应该包含日期或时间信息，但不应为空
  EXPECT_FALSE(result.empty()) << "Got: " << result;
  // 验证格式基本合理（至少有数字或冒号）
  bool hasDigit = false;
  for (char c : result) {
    if (std::isdigit(c)) {
      hasDigit = true;
      break;
    }
  }
  EXPECT_TRUE(hasDigit) << "Time format should contain digits, got: " << result;
}

TEST_F(FormatTest, FileFormatItem) {
  FileFormatItem item;
  fmt::memory_buffer buffer;
  item.format(buffer, *msg);

  std::string result(buffer.data(), buffer.size());
  EXPECT_EQ(result, "test_file.cc");
}

TEST_F(FormatTest, LineFormatItem) {
  LineFormatItem item;
  fmt::memory_buffer buffer;
  item.format(buffer, *msg);

  std::string result(buffer.data(), buffer.size());
  EXPECT_EQ(result, "42");
}

TEST_F(FormatTest, ThreadIdFormatItem) {
  ThreadIdFormatItem item;
  fmt::memory_buffer buffer;
  item.format(buffer, *msg);

  std::string result(buffer.data(), buffer.size());
  EXPECT_FALSE(result.empty());
}

TEST_F(FormatTest, LoggerFormatItem) {
  LoggerFormatItem item;
  fmt::memory_buffer buffer;
  item.format(buffer, *msg);

  std::string result(buffer.data(), buffer.size());
  EXPECT_EQ(result, "test_logger");
}

TEST_F(FormatTest, TabFormatItem) {
  TabFormatItem item;
  fmt::memory_buffer buffer;
  item.format(buffer, *msg);

  std::string result(buffer.data(), buffer.size());
  EXPECT_EQ(result, "\t");
}

TEST_F(FormatTest, NLineFormatItem) {
  NLineFormatItem item;
  fmt::memory_buffer buffer;
  item.format(buffer, *msg);

  std::string result(buffer.data(), buffer.size());
  EXPECT_EQ(result, "\n");
}

TEST_F(FormatTest, OtherFormatItem) {
  OtherFormatItem item("[PREFIX]");
  fmt::memory_buffer buffer;
  item.format(buffer, *msg);

  std::string result(buffer.data(), buffer.size());
  EXPECT_EQ(result, "[PREFIX]");
}

// ===================== Formatter Tests =====================

TEST_F(FormatTest, FormatterDefault) {
  Formatter formatter;
  fmt::memory_buffer buffer;
  formatter.format(buffer, *msg);

  std::string result(buffer.data(), buffer.size());
  EXPECT_THAT(result, ::testing::HasSubstr("test_logger"));
  EXPECT_THAT(result, ::testing::HasSubstr("test_file.cc:42"));
  EXPECT_THAT(result, ::testing::HasSubstr("INFO"));
  EXPECT_THAT(result, ::testing::HasSubstr("test message"));
  EXPECT_THAT(result, ::testing::EndsWith("\n"));
}

TEST_F(FormatTest, FormatterCustomPattern) {
  Formatter formatter("%p - %m%n");
  fmt::memory_buffer buffer;
  formatter.format(buffer, *msg);

  std::string result(buffer.data(), buffer.size());
  EXPECT_EQ(result, "INFO - test message\n");
}

TEST_F(FormatTest, FormatterSimpleMessage) {
  Formatter formatter("%m");
  fmt::memory_buffer buffer;
  formatter.format(buffer, *msg);

  std::string result(buffer.data(), buffer.size());
  EXPECT_EQ(result, "test message");
}

TEST_F(FormatTest, FormatterMultipleItems) {
  Formatter formatter("[%p][%c] %m%n");
  fmt::memory_buffer buffer;
  formatter.format(buffer, *msg);

  std::string result(buffer.data(), buffer.size());
  EXPECT_EQ(result, "[INFO][test_logger] test message\n");
}

TEST_F(FormatTest, FormatterEscapePercent) {
  Formatter formatter("100%% complete: %m");
  fmt::memory_buffer buffer;
  formatter.format(buffer, *msg);

  std::string result(buffer.data(), buffer.size());
  EXPECT_EQ(result, "100% complete: test message");
}

TEST_F(FormatTest, FormatterTimeWithSubPattern) {
  Formatter formatter("%d{%Y/%m/%d} %m");
  fmt::memory_buffer buffer;
  formatter.format(buffer, *msg);

  std::string result(buffer.data(), buffer.size());
  // 验证包含消息内容
  EXPECT_THAT(result, ::testing::HasSubstr("test message"));
  // 验证结果不为空且有数字（时间部分）
  EXPECT_FALSE(result.empty());
  bool hasDigit = false;
  for (char c : result) {
    if (std::isdigit(c)) {
      hasDigit = true;
      break;
    }
  }
  EXPECT_TRUE(hasDigit) << "Got: " << result;
}

TEST_F(FormatTest, FormatterTabIndent) {
  Formatter formatter("%p%T%m");
  fmt::memory_buffer buffer;
  formatter.format(buffer, *msg);

  std::string result(buffer.data(), buffer.size());
  EXPECT_EQ(result, "INFO\ttest message");
}

TEST_F(FormatTest, FormatterFileAndLine) {
  Formatter formatter("%f:%l");
  fmt::memory_buffer buffer;
  formatter.format(buffer, *msg);

  std::string result(buffer.data(), buffer.size());
  EXPECT_EQ(result, "test_file.cc:42");
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
