#include "znet/znet_logger.h"

#include "zco/zco_log.h"

#include <gtest/gtest.h>

namespace znet {
namespace {

class ZnetLoggerUnitTest : public ::testing::Test {};

TEST_F(ZnetLoggerUnitTest, GetLoggerLazilyInitializesDefaultLogger) {
    zlog::Logger::ptr logger = get_logger_ptr();
    ASSERT_NE(logger, nullptr);
    EXPECT_EQ(logger->get_name(), "znet_logger");
    EXPECT_NE(dynamic_cast<zlog::AsyncLogger *>(logger.get()), nullptr);
}

TEST_F(ZnetLoggerUnitTest, InitLoggerLevelOverloadWorks) {
    init_logger(zlog::LogLevel::value::DEBUG);
    zlog::Logger::ptr logger = get_logger_ptr();
    ASSERT_NE(logger, nullptr);
    EXPECT_EQ(logger->get_name(), "znet_logger");
    EXPECT_NE(dynamic_cast<zlog::AsyncLogger *>(logger.get()), nullptr);
    EXPECT_TRUE(should_log(zlog::LogLevel::value::DEBUG));

    ZNET_LOG_DEBUG("debug {}", 1);
    ZNET_LOG_WARN("warn {}", 2);
    ZNET_LOG_ERROR("error {}", 3);
    ZNET_LOG_FATAL("fatal {}", 4);
}

TEST_F(ZnetLoggerUnitTest, InitLoggerRefreshesCachedLogger) {
    init_logger(zlog::LogLevel::value::INFO);
    zlog::Logger::ptr first = get_logger_ptr();
    ASSERT_NE(first, nullptr);

    init_logger(zlog::LogLevel::value::OFF);
    zlog::Logger::ptr second = get_logger_ptr();
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first, second);
    EXPECT_EQ(second,
              zlog::LoggerManager::get_instance().get_logger("znet_logger"));
    EXPECT_FALSE(should_log(zlog::LogLevel::value::FATAL));
}

TEST_F(ZnetLoggerUnitTest, InitLoggerAlsoInitializesZcoLogger) {
    init_logger(zlog::LogLevel::value::ERROR);

    zlog::Logger::ptr co_logger = zco::get_logger_ptr();
    ASSERT_NE(co_logger, nullptr);
    EXPECT_EQ(co_logger->get_name(), "zco_logger");
    EXPECT_NE(dynamic_cast<zlog::AsyncLogger *>(co_logger.get()), nullptr);
}

} // namespace
} // namespace znet

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    znet::init_logger();
    return RUN_ALL_TESTS();
}
