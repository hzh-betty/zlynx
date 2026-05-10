#include "zhttp/zhttp_logger.h"

#include "zco/zco_log.h"
#include "znet/znet_logger.h"

#include <gtest/gtest.h>

TEST(ZhttpLoggerTest, LazyGetLoggerInitializesDefaultLogger) {
    zlog::Logger::ptr logger = zhttp::get_logger_ptr();
    ASSERT_NE(logger, nullptr);
    EXPECT_EQ(logger->get_name(), "zhttp_logger");
    EXPECT_NE(dynamic_cast<zlog::AsyncLogger *>(logger.get()), nullptr);
}

TEST(ZhttpLoggerTest, InitLoggerLevelOverloadWorks) {
    zhttp::init_logger(zlog::LogLevel::value::ERROR);
    zlog::Logger::ptr logger = zhttp::get_logger_ptr();
    ASSERT_NE(logger, nullptr);
    EXPECT_EQ(logger->get_name(), "zhttp_logger");
    EXPECT_NE(dynamic_cast<zlog::AsyncLogger *>(logger.get()), nullptr);
    EXPECT_FALSE(zhttp::should_log(zlog::LogLevel::value::WARNING));
    EXPECT_TRUE(zhttp::should_log(zlog::LogLevel::value::ERROR));

    ZHTTP_LOG_DEBUG("debug {}", 1);
    ZHTTP_LOG_INFO("info {}", 2);
    ZHTTP_LOG_WARN("warn {}", 3);
    ZHTTP_LOG_ERROR("error {}", 4);
    ZHTTP_LOG_FATAL("fatal {}", 5);
}

TEST(ZhttpLoggerTest, InitLoggerRefreshesCachedLogger) {
    zhttp::init_logger(zlog::LogLevel::value::INFO);
    zlog::Logger::ptr first = zhttp::get_logger_ptr();
    ASSERT_NE(first, nullptr);

    zhttp::init_logger(zlog::LogLevel::value::OFF);
    zlog::Logger::ptr second = zhttp::get_logger_ptr();
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first, second);
    EXPECT_EQ(second,
              zlog::LoggerManager::get_instance().get_logger("zhttp_logger"));
    EXPECT_FALSE(zhttp::should_log(zlog::LogLevel::value::FATAL));
}

TEST(ZhttpLoggerTest, InitLoggerAlsoInitializesDependencies) {
    zhttp::init_logger(zlog::LogLevel::value::WARNING);

    zlog::Logger::ptr co_logger = zco::get_logger_ptr();
    zlog::Logger::ptr net_logger = znet::get_logger_ptr();
    ASSERT_NE(co_logger, nullptr);
    ASSERT_NE(net_logger, nullptr);
    EXPECT_EQ(co_logger->get_name(), "zco_logger");
    EXPECT_EQ(net_logger->get_name(), "znet_logger");
    EXPECT_NE(dynamic_cast<zlog::AsyncLogger *>(co_logger.get()), nullptr);
    EXPECT_NE(dynamic_cast<zlog::AsyncLogger *>(net_logger.get()), nullptr);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    zhttp::init_logger();
    return RUN_ALL_TESTS();
}
