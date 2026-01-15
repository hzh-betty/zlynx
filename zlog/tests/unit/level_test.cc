#include "level.h"
#include <gtest/gtest.h>
#include <vector>

using namespace zlog;

class LevelTest : public ::testing::Test {};

TEST_F(LevelTest, ToStringDebug) {
  EXPECT_EQ(LogLevel::toString(LogLevel::value::DEBUG), "DEBUG");
}

TEST_F(LevelTest, ToStringInfo) {
  EXPECT_EQ(LogLevel::toString(LogLevel::value::INFO), "INFO");
}

TEST_F(LevelTest, ToStringWarning) {
  EXPECT_EQ(LogLevel::toString(LogLevel::value::WARNING), "WARNING");
}

TEST_F(LevelTest, ToStringError) {
  EXPECT_EQ(LogLevel::toString(LogLevel::value::ERROR), "ERROR");
}

TEST_F(LevelTest, ToStringFatal) {
  EXPECT_EQ(LogLevel::toString(LogLevel::value::FATAL), "FATAL");
}

TEST_F(LevelTest, ToStringUnknown) {
  EXPECT_EQ(LogLevel::toString(LogLevel::value::UNKNOWN), "UNKNOWN");
}

TEST_F(LevelTest, ToStringOff) {
  EXPECT_EQ(LogLevel::toString(LogLevel::value::OFF), "OFF");
}

TEST_F(LevelTest, ComparisonLessThan) {
  EXPECT_TRUE(LogLevel::value::DEBUG < LogLevel::value::INFO);
  EXPECT_TRUE(LogLevel::value::INFO < LogLevel::value::WARNING);
  EXPECT_TRUE(LogLevel::value::WARNING < LogLevel::value::ERROR);
  EXPECT_TRUE(LogLevel::value::ERROR < LogLevel::value::FATAL);
  EXPECT_TRUE(LogLevel::value::FATAL < LogLevel::value::OFF);

  EXPECT_FALSE(LogLevel::value::INFO < LogLevel::value::DEBUG);
  EXPECT_FALSE(LogLevel::value::ERROR < LogLevel::value::INFO);
}

TEST_F(LevelTest, ComparisonLessEqual) {
  EXPECT_TRUE(LogLevel::value::DEBUG <= LogLevel::value::DEBUG);
  EXPECT_TRUE(LogLevel::value::DEBUG <= LogLevel::value::INFO);
  EXPECT_TRUE(LogLevel::value::INFO <= LogLevel::value::WARNING);

  EXPECT_FALSE(LogLevel::value::WARNING <= LogLevel::value::INFO);
}

TEST_F(LevelTest, ComparisonGreaterThan) {
  EXPECT_TRUE(LogLevel::value::INFO > LogLevel::value::DEBUG);
  EXPECT_TRUE(LogLevel::value::ERROR > LogLevel::value::WARNING);
  EXPECT_TRUE(LogLevel::value::FATAL > LogLevel::value::ERROR);

  EXPECT_FALSE(LogLevel::value::DEBUG > LogLevel::value::INFO);
}

TEST_F(LevelTest, ComparisonGreaterEqual) {
  EXPECT_TRUE(LogLevel::value::INFO >= LogLevel::value::INFO);
  EXPECT_TRUE(LogLevel::value::ERROR >= LogLevel::value::INFO);

  EXPECT_FALSE(LogLevel::value::DEBUG >= LogLevel::value::INFO);
}

TEST_F(LevelTest, LevelOrdering) {
  std::vector<LogLevel::value> orderedLevels;
  orderedLevels.push_back(LogLevel::value::UNKNOWN);
  orderedLevels.push_back(LogLevel::value::DEBUG);
  orderedLevels.push_back(LogLevel::value::INFO);
  orderedLevels.push_back(LogLevel::value::WARNING);
  orderedLevels.push_back(LogLevel::value::ERROR);
  orderedLevels.push_back(LogLevel::value::FATAL);
  orderedLevels.push_back(LogLevel::value::OFF);

  for (size_t i = 0; i < orderedLevels.size() - 1; i++) {
    EXPECT_TRUE(orderedLevels[i] < orderedLevels[i + 1])
        << "Level " << LogLevel::toString(orderedLevels[i])
        << " should be less than " << LogLevel::toString(orderedLevels[i + 1]);
  }
}

TEST_F(LevelTest, EnumValues) {
  EXPECT_EQ(static_cast<int>(LogLevel::value::UNKNOWN), 0);
  EXPECT_EQ(static_cast<int>(LogLevel::value::DEBUG), 1);
  EXPECT_EQ(static_cast<int>(LogLevel::value::INFO), 2);
  EXPECT_EQ(static_cast<int>(LogLevel::value::WARNING), 3);
  EXPECT_EQ(static_cast<int>(LogLevel::value::ERROR), 4);
  EXPECT_EQ(static_cast<int>(LogLevel::value::FATAL), 5);
  EXPECT_EQ(static_cast<int>(LogLevel::value::OFF), 6);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
