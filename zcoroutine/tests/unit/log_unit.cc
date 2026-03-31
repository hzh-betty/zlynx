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

}  // namespace
}  // namespace zcoroutine
