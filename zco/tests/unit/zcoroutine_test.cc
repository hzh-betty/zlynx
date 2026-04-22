#include <type_traits>

#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zco/zco.h"

namespace zco {
namespace {

class ZcoroutineUnitTest : public test::RuntimeTestBase {};

TEST_F(ZcoroutineUnitTest, TypeAliasesAreUsable) {
    static_assert(std::is_same<channel<int>, Channel<int>>::value,
                  "channel alias should map to Channel<T>");
    static_assert(std::is_same<event, Event>::value,
                  "event alias should map to Event");
    static_assert(std::is_same<wait_group, WaitGroup>::value,
                  "wait_group alias should map to WaitGroup");
    static_assert(std::is_same<pool, Pool>::value,
                  "pool alias should map to Pool");
    static_assert(std::is_same<io_event, IoEvent>::value,
                  "io_event alias should map to IoEvent");
    static_assert(std::is_same<mutex, Mutex>::value,
                  "mutex alias should map to Mutex");
    static_assert(std::is_same<mutex_guard, MutexGuard>::value,
                  "mutex_guard alias should map to MutexGuard");

    SUCCEED();
}

} // namespace
} // namespace zco

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
