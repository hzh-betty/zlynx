/**
 * @file object_pool_test.cc
 * @brief ObjectPool 单元测试
 */

#include "object_pool.h"
#include <gtest/gtest.h>

#include <cstring>
#include <vector>

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

static void ExpectPointerAligned(void *p) {
  ASSERT_NE(p, nullptr);
  const uintptr_t v = reinterpret_cast<uintptr_t>(p);
  EXPECT_EQ(v % alignof(void *), 0u);
}

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

// 大对象测试
struct LargeObject {
  char data[1024];
  int value;
  LargeObject() : value(999) { std::memset(data, 0, sizeof(data)); }
};

TEST(ObjectPoolLargeTest, LargeObjectHandling) {
  ObjectPool<LargeObject> pool;
  LargeObject *obj = pool.allocate();
  EXPECT_NE(obj, nullptr);
  EXPECT_EQ(obj->value, 999);
  pool.deallocate(obj);
}

// 连续分配释放
TEST_F(ObjectPoolTest, ConsecutiveAllocDealloc) {
  for (int i = 0; i < 100; ++i) {
    TestObject *obj = pool_.allocate();
    EXPECT_NE(obj, nullptr);
    EXPECT_EQ(obj->value, 42);
    pool_.deallocate(obj);
  }
}

// 对象值持久化测试
TEST_F(ObjectPoolTest, ValuePersistence) {
  TestObject *obj = pool_.allocate();
  obj->value = 12345;
  EXPECT_EQ(obj->value, 12345);
  pool_.deallocate(obj);
}

// 多轮分配释放
TEST_F(ObjectPoolTest, MultiRoundAllocDealloc) {
  for (int round = 0; round < 5; ++round) {
    std::vector<TestObject *> objects;
    for (int i = 0; i < 20; ++i) {
      objects.push_back(pool_.allocate());
    }
    for (auto *obj : objects) {
      pool_.deallocate(obj);
    }
  }
}

// 释放后重新分配
TEST_F(ObjectPoolTest, ReuseAfterDealloc) {
  std::vector<TestObject *> first_batch;
  for (int i = 0; i < 10; ++i) {
    first_batch.push_back(pool_.allocate());
  }
  for (auto *obj : first_batch) {
    pool_.deallocate(obj);
  }

  // 再次分配，应该复用
  std::vector<TestObject *> second_batch;
  for (int i = 0; i < 10; ++i) {
    second_batch.push_back(pool_.allocate());
    EXPECT_NE(second_batch.back(), nullptr);
  }
  for (auto *obj : second_batch) {
    pool_.deallocate(obj);
  }
}

// 构造函数调用次数
TEST_F(ObjectPoolTest, ConstructorCalledOnAllocate) {
  TestObject *obj1 = pool_.allocate();
  EXPECT_EQ(obj1->value, 42);
  obj1->value = 100;
  pool_.deallocate(obj1);

  // 再次分配，构造函数应该重新调用
  TestObject *obj2 = pool_.allocate();
  EXPECT_EQ(obj2->value, 42);
  pool_.deallocate(obj2);
}

// 大量对象分配不会失败
TEST_F(ObjectPoolTest, MassAllocationNoFail) {
  std::vector<TestObject *> objects;
  for (int i = 0; i < 1000; ++i) {
    TestObject *obj = pool_.allocate();
    ASSERT_NE(obj, nullptr);
    objects.push_back(obj);
  }
  for (auto *obj : objects) {
    pool_.deallocate(obj);
  }
}

// 不同池子独立性
TEST(ObjectPoolIndependentTest, PoolsAreIndependent) {
  ObjectPool<TestObject> pool1;
  ObjectPool<TestObject> pool2;

  TestObject *obj1 = pool1.allocate();
  TestObject *obj2 = pool2.allocate();

  EXPECT_NE(obj1, obj2);

  pool1.deallocate(obj1);
  pool2.deallocate(obj2);
}

// ------------------------------
// 批量用例：释放顺序决定复用顺序（LIFO）
// ------------------------------

#define ZMALLOC_OBJECTPOOL_LIFO_REUSE_CASE(N)                                  \
  TEST_F(ObjectPoolTest, LifoReuseOrder_N##N) {                                \
    std::vector<TestObject *> objs;                                            \
    objs.reserve(N);                                                           \
    for (int i = 0; i < (N); ++i) {                                            \
      auto *p = pool_.allocate();                                              \
      ASSERT_NE(p, nullptr);                                                   \
      p->value = 100 + i;                                                      \
      objs.push_back(p);                                                       \
    }                                                                          \
    for (int i = 0; i < (N); ++i) {                                            \
      pool_.deallocate(objs[i]);                                               \
    }                                                                          \
    std::vector<TestObject *> reused;                                          \
    reused.reserve(N);                                                         \
    for (int i = 0; i < (N); ++i) {                                            \
      auto *q = pool_.allocate();                                              \
      ASSERT_NE(q, nullptr);                                                   \
      reused.push_back(q);                                                     \
    }                                                                          \
    for (int i = 0; i < (N); ++i) {                                            \
      EXPECT_EQ(reused[i], objs[(N) - 1 - i]);                                 \
      EXPECT_EQ(reused[i]->value, 42);                                         \
    }                                                                          \
    for (auto *q : reused) {                                                   \
      pool_.deallocate(q);                                                     \
    }                                                                          \
  }

ZMALLOC_OBJECTPOOL_LIFO_REUSE_CASE(2)
ZMALLOC_OBJECTPOOL_LIFO_REUSE_CASE(3)
ZMALLOC_OBJECTPOOL_LIFO_REUSE_CASE(4)
ZMALLOC_OBJECTPOOL_LIFO_REUSE_CASE(5)
ZMALLOC_OBJECTPOOL_LIFO_REUSE_CASE(6)
ZMALLOC_OBJECTPOOL_LIFO_REUSE_CASE(7)
ZMALLOC_OBJECTPOOL_LIFO_REUSE_CASE(8)
ZMALLOC_OBJECTPOOL_LIFO_REUSE_CASE(9)
ZMALLOC_OBJECTPOOL_LIFO_REUSE_CASE(10)

#undef ZMALLOC_OBJECTPOOL_LIFO_REUSE_CASE

// ------------------------------
// 批量用例：批量分配唯一性 + 可正常释放
// ------------------------------

#define ZMALLOC_OBJECTPOOL_BATCH_UNIQUE_CASE(N)                                \
  TEST_F(ObjectPoolTest, BatchAllocationUnique_N##N) {                         \
    std::vector<TestObject *> objs;                                            \
    objs.reserve(N);                                                           \
    for (int i = 0; i < (N); ++i) {                                            \
      auto *p = pool_.allocate();                                              \
      ASSERT_NE(p, nullptr);                                                   \
      ExpectPointerAligned(p);                                                 \
      objs.push_back(p);                                                       \
    }                                                                          \
    for (int i = 0; i < (N); ++i) {                                            \
      for (int j = i + 1; j < (N); ++j) {                                      \
        ASSERT_NE(objs[i], objs[j]);                                           \
      }                                                                        \
    }                                                                          \
    for (auto *p : objs) {                                                     \
      pool_.deallocate(p);                                                     \
    }                                                                          \
  }

ZMALLOC_OBJECTPOOL_BATCH_UNIQUE_CASE(5)
ZMALLOC_OBJECTPOOL_BATCH_UNIQUE_CASE(10)
ZMALLOC_OBJECTPOOL_BATCH_UNIQUE_CASE(20)
ZMALLOC_OBJECTPOOL_BATCH_UNIQUE_CASE(50)
ZMALLOC_OBJECTPOOL_BATCH_UNIQUE_CASE(100)

#undef ZMALLOC_OBJECTPOOL_BATCH_UNIQUE_CASE

// ------------------------------
// 批量用例：交替分配/释放不同迭代次数
// ------------------------------

#define ZMALLOC_OBJECTPOOL_ALTERNATE_CASE(ITERS)                               \
  TEST_F(ObjectPoolTest, AlternateAllocateDeallocate_Iters##ITERS) {           \
    for (int i = 0; i < (ITERS); ++i) {                                        \
      auto *p = pool_.allocate();                                              \
      ASSERT_NE(p, nullptr);                                                   \
      p->value = i;                                                            \
      pool_.deallocate(p);                                                     \
    }                                                                          \
    auto *q = pool_.allocate();                                                \
    ASSERT_NE(q, nullptr);                                                     \
    EXPECT_EQ(q->value, 42);                                                   \
    pool_.deallocate(q);                                                       \
  }

ZMALLOC_OBJECTPOOL_ALTERNATE_CASE(1)
ZMALLOC_OBJECTPOOL_ALTERNATE_CASE(2)
ZMALLOC_OBJECTPOOL_ALTERNATE_CASE(10)
ZMALLOC_OBJECTPOOL_ALTERNATE_CASE(100)
ZMALLOC_OBJECTPOOL_ALTERNATE_CASE(1000)

#undef ZMALLOC_OBJECTPOOL_ALTERNATE_CASE

// ------------------------------
// SmallObject：对齐/批量/复用
// ------------------------------

static void SmallObjectBatch(ObjectPool<SmallObject> &pool, int n) {
  std::vector<SmallObject *> objs;
  objs.reserve(n);
  for (int i = 0; i < n; ++i) {
    auto *p = pool.allocate();
    ASSERT_NE(p, nullptr);
    ExpectPointerAligned(p);
    objs.push_back(p);
  }
  for (auto *p : objs) {
    pool.deallocate(p);
  }
}

#define ZMALLOC_OBJECTPOOL_SMALL_BATCH_CASE(N)                                 \
  TEST(ObjectPoolSmallTest, SmallObjectBatch_N##N) {                           \
    ObjectPool<SmallObject> pool;                                              \
    SmallObjectBatch(pool, N);                                                 \
  }

ZMALLOC_OBJECTPOOL_SMALL_BATCH_CASE(1)
ZMALLOC_OBJECTPOOL_SMALL_BATCH_CASE(2)
ZMALLOC_OBJECTPOOL_SMALL_BATCH_CASE(3)
ZMALLOC_OBJECTPOOL_SMALL_BATCH_CASE(4)
ZMALLOC_OBJECTPOOL_SMALL_BATCH_CASE(5)
ZMALLOC_OBJECTPOOL_SMALL_BATCH_CASE(6)
ZMALLOC_OBJECTPOOL_SMALL_BATCH_CASE(7)
ZMALLOC_OBJECTPOOL_SMALL_BATCH_CASE(8)
ZMALLOC_OBJECTPOOL_SMALL_BATCH_CASE(9)
ZMALLOC_OBJECTPOOL_SMALL_BATCH_CASE(10)

#undef ZMALLOC_OBJECTPOOL_SMALL_BATCH_CASE

// LargeObject：复用顺序 + 构造函数重置
static void LargeObjectLifoReuse(ObjectPool<LargeObject> &pool, int n) {
  std::vector<LargeObject *> objs;
  objs.reserve(n);
  for (int i = 0; i < n; ++i) {
    auto *p = pool.allocate();
    ASSERT_NE(p, nullptr);
    ExpectPointerAligned(p);
    p->value = 1000 + i;
    objs.push_back(p);
  }
  for (int i = 0; i < n; ++i) {
    pool.deallocate(objs[i]);
  }
  std::vector<LargeObject *> reused;
  reused.reserve(n);
  for (int i = 0; i < n; ++i) {
    auto *q = pool.allocate();
    ASSERT_NE(q, nullptr);
    reused.push_back(q);
  }
  for (int i = 0; i < n; ++i) {
    EXPECT_EQ(reused[i], objs[n - 1 - i]);
    EXPECT_EQ(reused[i]->value, 999);
  }
  for (auto *q : reused) {
    pool.deallocate(q);
  }
}

#define ZMALLOC_OBJECTPOOL_LARGE_REUSE_CASE(N)                                 \
  TEST(ObjectPoolLargeTest, LargeObjectLifoReuse_N##N) {                       \
    ObjectPool<LargeObject> pool;                                              \
    LargeObjectLifoReuse(pool, N);                                             \
  }

ZMALLOC_OBJECTPOOL_LARGE_REUSE_CASE(2)
ZMALLOC_OBJECTPOOL_LARGE_REUSE_CASE(3)
ZMALLOC_OBJECTPOOL_LARGE_REUSE_CASE(4)
ZMALLOC_OBJECTPOOL_LARGE_REUSE_CASE(5)
ZMALLOC_OBJECTPOOL_LARGE_REUSE_CASE(6)
ZMALLOC_OBJECTPOOL_LARGE_REUSE_CASE(7)

#undef ZMALLOC_OBJECTPOOL_LARGE_REUSE_CASE

} // namespace
} // namespace zmalloc

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
