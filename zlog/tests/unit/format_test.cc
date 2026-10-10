#include "zlog/format.h"
#include "zlog/message.h"
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

TEST_F(FormatTest, MessageOutput) {
    Formatter item("%m");
    fmt::memory_buffer buffer;
    item.format(buffer, *msg);

    std::string result(buffer.data(), buffer.size());
    EXPECT_EQ(result, "test message");
}

TEST_F(FormatTest, MessageOutputEmpty) {
    LogMessage emptyMsg(LogLevel::value::INFO, "test.cc", 1, "", "logger");
    Formatter item("%m");
    fmt::memory_buffer buffer;
    item.format(buffer, emptyMsg);

    std::string result(buffer.data(), buffer.size());
    EXPECT_EQ(result, "");
}

TEST_F(FormatTest, MessageOutputNullPayload) {
    LogMessage nullMsg(LogLevel::value::INFO, "test.cc", 1, nullptr, "logger");
    Formatter item("%m");
    fmt::memory_buffer buffer;
    item.format(buffer, nullMsg);

    std::string result(buffer.data(), buffer.size());
    EXPECT_EQ(result, "");
}

TEST_F(FormatTest, LevelOutput) {
    Formatter item("%p");
    fmt::memory_buffer buffer;
    item.format(buffer, *msg);

    std::string result(buffer.data(), buffer.size());
    EXPECT_EQ(result, "INFO");
}

TEST_F(FormatTest, LevelOutputAllLevels) {
    Formatter item("%p");

    std::vector<std::pair<LogLevel::value, std::string>> levels;
    levels.push_back(
        std::make_pair(LogLevel::value::DEBUG, std::string("DEBUG")));
    levels.push_back(
        std::make_pair(LogLevel::value::INFO, std::string("INFO")));
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

TEST_F(FormatTest, TimeOutputDefault) {
    Formatter item("%d");
    fmt::memory_buffer buffer;
    item.format(buffer, *msg);

    std::string result(buffer.data(), buffer.size());
    // 默认格式 %H:%M:%S，应匹配 HH:MM:SS 格式
    std::regex timePattern(R"(\d{2}:\d{2}:\d{2})");
    EXPECT_TRUE(std::regex_match(result, timePattern)) << "Got: " << result;
}

TEST_F(FormatTest, TimeOutputCustom) {
    Formatter item("%d{%Y-%m-%d}");
    msg->curtime_ = 1700000000;
    fmt::memory_buffer buffer;
    item.format(buffer, *msg);

    std::string result(buffer.data(), buffer.size());
    const std::regex date_pattern(R"(\d{4}-\d{2}-\d{2})");
    EXPECT_TRUE(std::regex_match(result, date_pattern)) << "Got: " << result;
}

TEST_F(FormatTest, TimeOutputOverflowFallback) {
    Formatter item("%d{" + std::string(64, 'x') + "}");
    LogMessage testMsg(LogLevel::value::INFO, "test.cc", 1, "payload",
                       "logger");
    testMsg.curtime_ = msg->curtime_ + 1;
    fmt::memory_buffer buffer;
    item.format(buffer, testMsg);

    std::string result(buffer.data(), buffer.size());
    EXPECT_EQ(result, "InvalidTime");
}

TEST_F(FormatTest, FileOutput) {
    Formatter item("%f");
    fmt::memory_buffer buffer;
    item.format(buffer, *msg);

    std::string result(buffer.data(), buffer.size());
    EXPECT_EQ(result, "test_file.cc");
}

TEST_F(FormatTest, FileOutputNullFile) {
    LogMessage nullMsg(LogLevel::value::INFO, nullptr, 1, "payload", "logger");
    Formatter item("%f");
    fmt::memory_buffer buffer;
    item.format(buffer, nullMsg);

    std::string result(buffer.data(), buffer.size());
    EXPECT_EQ(result, "");
}

TEST_F(FormatTest, LineOutput) {
    Formatter item("%l");
    fmt::memory_buffer buffer;
    item.format(buffer, *msg);

    std::string result(buffer.data(), buffer.size());
    EXPECT_EQ(result, "42");
}

TEST_F(FormatTest, ThreadIdOutput) {
    Formatter item("%t");
    fmt::memory_buffer buffer;
    item.format(buffer, *msg);

    std::string result(buffer.data(), buffer.size());
    EXPECT_FALSE(result.empty());
}

TEST_F(FormatTest, ThreadIdOutputCacheFastPathOnSameThreadId) {
    Formatter item("%t");
    fmt::memory_buffer first;
    fmt::memory_buffer second;

    item.format(first, *msg);
    item.format(second, *msg);

    const std::string first_str(first.data(), first.size());
    const std::string second_str(second.data(), second.size());
    EXPECT_EQ(first_str, second_str);
    EXPECT_FALSE(first_str.empty());
}

TEST_F(FormatTest, LoggerOutput) {
    Formatter item("%c");
    fmt::memory_buffer buffer;
    item.format(buffer, *msg);

    std::string result(buffer.data(), buffer.size());
    EXPECT_EQ(result, "test_logger");
}

TEST_F(FormatTest, LoggerOutputNullLoggerName) {
    LogMessage nullMsg(LogLevel::value::INFO, "file.cc", 1, "payload", nullptr);
    Formatter item("%c");
    fmt::memory_buffer buffer;
    item.format(buffer, nullMsg);

    std::string result(buffer.data(), buffer.size());
    EXPECT_EQ(result, "");
}

TEST_F(FormatTest, TabOutput) {
    Formatter item("%T");
    fmt::memory_buffer buffer;
    item.format(buffer, *msg);

    std::string result(buffer.data(), buffer.size());
    EXPECT_EQ(result, "\t");
}

TEST_F(FormatTest, NLineOutput) {
    Formatter item("%n");
    fmt::memory_buffer buffer;
    item.format(buffer, *msg);

    std::string result(buffer.data(), buffer.size());
    EXPECT_EQ(result, "\n");
}

TEST_F(FormatTest, LiteralOutput) {
    Formatter item("[PREFIX]%m");
    msg->payload_ = "";
    fmt::memory_buffer buffer;
    item.format(buffer, *msg);

    std::string result(buffer.data(), buffer.size());
    EXPECT_EQ(result, "[PREFIX]");
}

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

TEST_F(FormatTest, FormatterInvalidTrailingPercentPattern) {
    EXPECT_THROW(Formatter("prefix%"), std::invalid_argument);
}

TEST_F(FormatTest, FormatterInvalidUnclosedSubPattern) {
    EXPECT_THROW(Formatter("%d{%Y-%m-%d"), std::invalid_argument);
}

TEST_F(FormatTest, FormatterUnknownPatternItemThrows) {
    EXPECT_THROW(Formatter("%x"), std::invalid_argument);
}

TEST_F(FormatTest, FormatterPreservesTrailingLiteralsAndPercent) {
    // 普通文本在解析结束时也必须保存。
    for (const auto &pattern : {std::string("literal"), std::string("[%m] END"),
                               std::string("%m%%")}) {
        Formatter formatter(pattern);
        fmt::memory_buffer buffer;
        formatter.format(buffer, *msg);
        const std::string result(buffer.data(), buffer.size());
        if (pattern == "literal") {
            EXPECT_EQ(result, "literal");
        } else if (pattern == "[%m] END") {
            EXPECT_EQ(result, "[test message] END");
        } else {
            EXPECT_EQ(result, "test message%");
        }
    }
}

TEST_F(FormatTest, TimeCacheSeparatesFormatsAndFormattersWithinOneSecond) {
    msg->curtime_ = 1700000000;
    Formatter year("%d{%Y}"), month("%d{%m}"), both("%d{%Y}|%d{%m}");
    for (int i = 0; i < 3; ++i) {
        fmt::memory_buffer first, second, combined;
        year.format(first, *msg);
        month.format(second, *msg);
        both.format(combined, *msg);
        EXPECT_EQ(std::string(first.data(), first.size()), "2023");
        EXPECT_EQ(std::string(second.data(), second.size()), "11");
        EXPECT_EQ(std::string(combined.data(), combined.size()), "2023|11");
    }
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
