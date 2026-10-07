#include "zlog/module_logger.h"

#include <gtest/gtest.h>

namespace zlog {
namespace {

int dependency_init_calls = 0;
LogLevel::value dependency_level = LogLevel::value::OFF;

void init_dependency(LogLevel::value level) {
    ++dependency_init_calls;
    dependency_level = level;
}

TEST(ModuleLoggerTest, LazyInitializationCachesAndInitializesDependencies) {
    dependency_init_calls = 0;
    ModuleLogger logger("module_lazy_test", init_dependency);
    const auto first = logger.get_logger_ptr();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->get_name(), "module_lazy_test");
    EXPECT_EQ(logger.get_logger_ptr(), first);
    EXPECT_EQ(LoggerManager::get_instance().get_logger("module_lazy_test"), first);
    EXPECT_EQ(dependency_init_calls, 1);
    EXPECT_EQ(dependency_level, LogLevel::value::INFO);
}

TEST(ModuleLoggerTest, ReusesRegisteredLoggerWithoutInitializingDependencies) {
    LoggerBuilder builder;
    builder.build_logger_name("module_registered_test");
    const auto registered = builder.build();
    LoggerManager::get_instance().upsert_logger("module_registered_test", registered);
    dependency_init_calls = 0;
    ModuleLogger logger("module_registered_test", init_dependency);
    EXPECT_EQ(logger.get_logger_ptr(), registered);
    EXPECT_EQ(dependency_init_calls, 0);
}

TEST(ModuleLoggerTest, ReinitializationReplacesLoggerAndPreservesLevelFiltering) {
    dependency_init_calls = 0;
    ModuleLogger logger("module_reinit_test", init_dependency);
    EXPECT_FALSE(logger.should_log(LogLevel::value::DEBUG));
    logger.init(LogLevel::value::WARNING);
    const auto first = logger.get_logger_ptr();
    EXPECT_FALSE(logger.should_log(LogLevel::value::INFO));
    EXPECT_TRUE(logger.should_log(LogLevel::value::ERROR));
    logger.init(LogLevel::value::OFF);
    EXPECT_NE(logger.get_logger_ptr(), first);
    EXPECT_EQ(LoggerManager::get_instance().get_logger("module_reinit_test"),
              logger.get_logger_ptr());
    EXPECT_FALSE(logger.should_log(LogLevel::value::FATAL));
    EXPECT_EQ(dependency_init_calls, 2);
    EXPECT_EQ(dependency_level, LogLevel::value::OFF);
}

} // namespace
} // namespace zlog

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
