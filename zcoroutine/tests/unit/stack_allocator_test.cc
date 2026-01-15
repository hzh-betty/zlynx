#include "runtime/stack_allocator.h"
#include "util/zcoroutine_logger.h"
#include <gtest/gtest.h>

using namespace zcoroutine;

class StackAllocatorTest : public ::testing::Test {
protected:
  void SetUp() override {
    // 可以在这里初始化资源，虽然 StackAllocator 是静态类
  }
  void TearDown() override {
    // 清理资源
  }
};

TEST_F(StackAllocatorTest, AllocateZero) {
  void *ptr = StackAllocator::allocate(0);
  EXPECT_EQ(ptr, nullptr);
}

TEST_F(StackAllocatorTest, DeallocateNull) {
  // Should not crash
  StackAllocator::deallocate(nullptr, 0);
  StackAllocator::deallocate(nullptr, 100);
}

int main(int argc, char **argv) {
  zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
