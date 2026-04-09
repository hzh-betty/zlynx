#include <gtest/gtest.h>

#include "zcoroutine/log.h"
#include "support/test_fixture.h"

namespace zcoroutine {
namespace {

class LogUnitByHeaderTest : public test::RuntimeTestBase {};

TEST_F(LogUnitByHeaderTest, DefaultLoggerAndMacrosAreCallable) {
  auto logger = default_logger();
  EXPECT_NE(logger, nullptr);

  ZCOROUTINE_LOG_DEBUG("log_unit debug {}", 1);
  ZCOROUTINE_LOG_INFO("log_unit info {}", 2);
  ZCOROUTINE_LOG_WARN("log_unit warn {}", 3);
  ZCOROUTINE_LOG_ERROR("log_unit error {}", 4);
}

TEST_F(LogUnitByHeaderTest, LoggerInitContractsStayCallableWithoutZlog) {
  LoggerInitOptions options;
  options.level = zlog::LogLevel::value::ERROR;
  options.async = false;
  options.formatter = kDefaultFormatter;
  options.sink = "stdout";

  init_logger(options);
  auto logger = default_logger();
  ASSERT_NE(logger, nullptr);
  EXPECT_EQ(logger.get(), get_logger());

  init_logger(zlog::LogLevel::value::WARNING);
  EXPECT_EQ(default_logger().get(), get_logger());

  ZCOROUTINE_LOG_FATAL("log_unit fatal {}", 5);
}

}  // namespace
}  // namespace zcoroutine
