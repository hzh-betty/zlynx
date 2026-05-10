#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zco/zco_log.h"
#include "zlog/logger.h"

namespace zco {
namespace {

class LogUnitByHeaderTest : public test::RuntimeTestBase {};

TEST_F(LogUnitByHeaderTest, DefaultLoggerAndMacrosAreCallable) {
    auto logger = get_logger_ptr();
    EXPECT_NE(logger, nullptr);

    ZCO_LOG_DEBUG("log_unit debug {}", 1);
    ZCO_LOG_INFO("log_unit info {}", 2);
    ZCO_LOG_WARN("log_unit warn {}", 3);
    ZCO_LOG_ERROR("log_unit error {}", 4);
}

TEST_F(LogUnitByHeaderTest, LoggerInitContractsRegisterInZlogManager) {
    init_logger(zlog::LogLevel::value::ERROR);
    auto logger = get_logger_ptr();
    ASSERT_NE(logger, nullptr);
    EXPECT_EQ(logger,
              zlog::LoggerManager::get_instance().get_logger(kLoggerName));
    EXPECT_NE(dynamic_cast<zlog::AsyncLogger *>(logger.get()), nullptr);
    EXPECT_FALSE(should_log(zlog::LogLevel::value::WARNING));
    EXPECT_TRUE(should_log(zlog::LogLevel::value::ERROR));

    init_logger(zlog::LogLevel::value::WARNING);
    EXPECT_EQ(get_logger_ptr(),
              zlog::LoggerManager::get_instance().get_logger(kLoggerName));
    EXPECT_TRUE(zlog::LoggerManager::get_instance().has_logger(kLoggerName));

    ZCO_LOG_FATAL("log_unit fatal {}", 5);
}

TEST_F(LogUnitByHeaderTest, InitLoggerRefreshesCachedLogger) {
    init_logger(zlog::LogLevel::value::INFO);
    auto first = get_logger_ptr();
    ASSERT_NE(first, nullptr);

    init_logger(zlog::LogLevel::value::OFF);
    auto second = get_logger_ptr();
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first, second);
    EXPECT_EQ(second,
              zlog::LoggerManager::get_instance().get_logger(kLoggerName));
    EXPECT_FALSE(should_log(zlog::LogLevel::value::FATAL));
}

TEST_F(LogUnitByHeaderTest, LoggerLevelAllowsMacroCalls) {
    init_logger(zlog::LogLevel::value::ERROR);

    ZCO_LOG_DEBUG("filtered debug {}", 1);
    ZCO_LOG_ERROR("visible error {}", 2);

    init_logger(zlog::LogLevel::value::OFF);
    ZCO_LOG_FATAL("filtered fatal {}", 3);
}

} // namespace
} // namespace zco

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    zco::init_logger();
    return RUN_ALL_TESTS();
}
