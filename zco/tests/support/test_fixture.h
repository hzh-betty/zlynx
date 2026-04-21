#ifndef ZCO_TESTS_SUPPORT_TEST_FIXTURE_H_
#define ZCO_TESTS_SUPPORT_TEST_FIXTURE_H_

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "zco/zco.h"
#include "zco/zco_log.h"

namespace zco {
namespace test {

class RuntimeTestBase : public ::testing::Test {
  protected:
    static zlog::LogLevel::value ParseLogLevelFromEnv() {
        const char *raw = std::getenv("ZCO_TEST_LOG_LEVEL");
        if (!raw) {
            return zlog::LogLevel::value::INFO;
        }

        std::string level(raw);
        std::transform(level.begin(), level.end(), level.begin(),
                       [](unsigned char ch) {
                           return static_cast<char>(std::tolower(ch));
                       });
        if (level == "debug") {
            return zlog::LogLevel::value::DEBUG;
        }
        if (level == "warning" || level == "warn") {
            return zlog::LogLevel::value::WARNING;
        }
        if (level == "error") {
            return zlog::LogLevel::value::ERROR;
        }
        if (level == "fatal") {
            return zlog::LogLevel::value::FATAL;
        }
        return zlog::LogLevel::value::INFO;
    }

    void SetUp() override {
        shutdown();
        co_stack_num(8);
        co_stack_size(128 * 1024);
        co_stack_model(StackModel::kShared);

        LoggerInitOptions options;
        options.level = ParseLogLevelFromEnv();
        options.async = false;
        options.sink = "stdout";
        init_logger(options);
    }

    void TearDown() override { shutdown(); }

    static void WaitShortly() {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
};

} // namespace test
} // namespace zco

#endif // ZCO_TESTS_SUPPORT_TEST_FIXTURE_H_
