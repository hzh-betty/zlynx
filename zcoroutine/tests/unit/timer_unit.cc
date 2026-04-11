#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zcoroutine/internal/timer.h"

namespace zcoroutine {
namespace {

class TimerUnitByHeaderTest : public test::RuntimeTestBase {};

TEST_F(TimerUnitByHeaderTest, NowMsIsMonotonic) {
    const uint64_t begin = now_ms();
    uint64_t end = begin;

    for (int i = 0; i < 10000 && end == begin; ++i) {
        end = now_ms();
    }

    EXPECT_GE(end, begin);
}

} // namespace
} // namespace zcoroutine
