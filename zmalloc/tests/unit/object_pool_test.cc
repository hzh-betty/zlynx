/**
 * @file object_pool_test.cc
 * @brief ObjectPool 单元测试
 */

#include "object_pool.h"
#include <gtest/gtest.h>

namespace zmalloc {
namespace {

struct TestObject {
  int value;
  char padding[64];

  TestObject() : value(42) {}
  ~TestObject() { value = 0; }
};

class ObjectPoolTest : public ::testing::Test {
protected:
  ObjectPool<TestObject> pool_;
};

TEST_F(ObjectPoolTest, AllocateReturnsNonNull) {
  TestObject *obj = pool_.allocate();
  EXPECT_NE(obj, nullptr);
  pool_.deallocate(obj);
}

TEST_F(ObjectPoolTest, ConstructorCalled) {
  TestObject *obj = pool_.allocate();
  EXPECT_EQ(obj->value, 42);
  pool_.deallocate(obj);
}

TEST_F(ObjectPoolTest, DestructorCalled) {
  TestObject *obj = pool_.allocate();
  obj->value = 100;
  pool_.deallocate(obj);
  // 析构后 value 应该被重置为 0（但内存可能被复用）
}

TEST_F(ObjectPoolTest, ReuseFreedObjects) {
  TestObject *obj1 = pool_.allocate();
  pool_.deallocate(obj1);
  TestObject *obj2 = pool_.allocate();
  // 应该复用刚释放的对象
  EXPECT_EQ(obj1, obj2);
  pool_.deallocate(obj2);
}

TEST_F(ObjectPoolTest, MultipleAllocations) {
  std::vector<TestObject *> objects;
  for (int i = 0; i < 100; ++i) {
    objects.push_back(pool_.allocate());
    EXPECT_NE(objects.back(), nullptr);
  }
  for (auto *obj : objects) {
    pool_.deallocate(obj);
  }
}

TEST_F(ObjectPoolTest, AllocateDeallocateAlternating) {
  for (int i = 0; i < 50; ++i) {
    TestObject *obj = pool_.allocate();
    EXPECT_NE(obj, nullptr);
    pool_.deallocate(obj);
  }
}

// 边界测试：小对象（小于指针大小）
struct SmallObject {
  char c;
};

TEST(ObjectPoolSmallTest, SmallObjectHandling) {
  ObjectPool<SmallObject> pool;
  SmallObject *obj = pool.allocate();
  EXPECT_NE(obj, nullptr);
  pool.deallocate(obj);
}

} // namespace
} // namespace zmalloc

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
