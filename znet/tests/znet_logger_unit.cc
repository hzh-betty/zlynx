#include "znet/znet_logger.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

namespace znet {
namespace {

std::string make_temp_log_file() {
    char dir_template[] = "/tmp/znet-log-XXXXXX";
    char *dir = ::mkdtemp(dir_template);
    EXPECT_NE(dir, nullptr);
    return std::string(dir ? dir : "/tmp") + "/znet.log";
}

TEST(ZnetLoggerUnitTest, GetLoggerLazilyInitializesDefaultLogger) {
    zlog::Logger *logger = get_logger();
    ASSERT_NE(logger, nullptr);
}

TEST(ZnetLoggerUnitTest, InitLoggerSupportsFileSinkAndCustomFormatter) {
    LoggerInitOptions options;
    options.async = false;
    options.level = zlog::LogLevel::value::INFO;
    options.formatter = "[%p]%m%n";
    options.sink = "FiLe";
    options.file_path = make_temp_log_file();

    init_logger(options);
    zlog::Logger *logger = get_logger();
    ASSERT_NE(logger, nullptr);

    ZNET_LOG_INFO("file sink log {}", 1);

    struct stat st;
    ASSERT_EQ(::stat(options.file_path.c_str(), &st), 0);
    EXPECT_GT(static_cast<long long>(st.st_size), 0);
}

TEST(ZnetLoggerUnitTest, InitLoggerSupportsBothSinkAliases) {
    LoggerInitOptions options;
    options.async = false;
    options.sink = "stdout+file";
    options.file_path = make_temp_log_file();
    init_logger(options);
    ASSERT_NE(get_logger(), nullptr);

    options.sink = "file+stdout";
    init_logger(options);
    ASSERT_NE(get_logger(), nullptr);

    options.sink = "both";
    init_logger(options);
    ASSERT_NE(get_logger(), nullptr);
}

TEST(ZnetLoggerUnitTest, InitLoggerLevelOverloadWorks) {
    init_logger(zlog::LogLevel::value::DEBUG);
    ASSERT_NE(get_logger(), nullptr);

    ZNET_LOG_DEBUG("debug {}", 1);
    ZNET_LOG_WARN("warn {}", 2);
    ZNET_LOG_ERROR("error {}", 3);
    ZNET_LOG_FATAL("fatal {}", 4);
}

TEST(ZnetLoggerUnitTest, EmptyFilePathFallsBackToDefaultPath) {
    LoggerInitOptions options;
    options.async = false;
    options.sink = "file";
    options.file_path.clear();
    init_logger(options);
    ASSERT_NE(get_logger(), nullptr);
}

} // namespace
} // namespace znet
