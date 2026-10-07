/**
 * @file fiber_handle_registry_test.cc
 * @brief 单元测试。
 * @author hzh-betty
 */

#include <gtest/gtest.h>

#include "support/internal_fiber_test_helper.h"
#include "support/test_fixture.h"
#include "zco/internal/fiber_handle_registry.h"
#include "zco/internal/processor.h"

namespace zco {
namespace {

class FiberHandleRegistryUnitTest : public test::RuntimeTestBase {};

TEST_F(FiberHandleRegistryUnitTest, RegisterLookupAndUnregisterFlow) {
    FiberHandleRegistry registry;
    Processor processor(0, 64 * 1024);

    Fiber::ptr fiber = test::MakeFiberForTest(&processor, 1, 0);

    registry.register_fiber(fiber, 101);

    Fiber::ptr found = registry.find_by_handle(101);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found.get(), fiber.get());

    EXPECT_EQ(fiber->external_handle_id(), 101u);

    registry.unregister_fiber(fiber.get());
    EXPECT_EQ(registry.find_by_handle(101), nullptr);
    EXPECT_EQ(fiber->external_handle_id(), 0u);
}

TEST_F(FiberHandleRegistryUnitTest, ExistingReverseMappingKeepsOriginalHandle) {
    FiberHandleRegistry registry;
    Processor processor(0, 64 * 1024);

    Fiber::ptr fiber = test::MakeFiberForTest(&processor, 7, 0);
    registry.register_fiber(fiber, 1001);
    registry.register_fiber(fiber, 2002);

    EXPECT_EQ(fiber->external_handle_id(), 1001u);

    EXPECT_NE(registry.find_by_handle(1001), nullptr);
    EXPECT_EQ(registry.find_by_handle(2002), nullptr);
}

TEST_F(FiberHandleRegistryUnitTest, InvalidInputsAndClearAreSafe) {
    FiberHandleRegistry registry;
    Processor processor(0, 64 * 1024);

    Fiber::ptr fiber = test::MakeFiberForTest(&processor, 8, 0);

    registry.register_fiber(nullptr, 100);
    registry.register_fiber(fiber, 0);
    EXPECT_EQ(registry.find_by_handle(0), nullptr);

    registry.register_fiber(fiber, 321);
    EXPECT_NE(registry.find_by_handle(321), nullptr);

    registry.unregister_fiber(nullptr);
    registry.clear();

    EXPECT_EQ(registry.find_by_handle(321), nullptr);
}

TEST_F(FiberHandleRegistryUnitTest, DoubleUnregisterIsSafe) {
    FiberHandleRegistry registry;
    Processor processor(0, 64 * 1024);
    Fiber::ptr fiber = test::MakeFiberForTest(&processor, 9, 0);
    ASSERT_NE(fiber, nullptr);

    ASSERT_EQ(registry.register_fiber(fiber, 777), 777u);
    EXPECT_EQ(registry.unregister_fiber(fiber.get()), 777u);
    EXPECT_EQ(registry.unregister_fiber(fiber.get()), 0u);
    EXPECT_EQ(fiber->external_handle_id(), 0u);
}

} // namespace
} // namespace zco

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    zco::init_logger();
    return RUN_ALL_TESTS();
}
