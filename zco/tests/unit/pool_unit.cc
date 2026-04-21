#include <atomic>
#include <thread>

#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zco/pool.h"

namespace zco {
namespace {

class PoolUnitByHeaderTest : public test::RuntimeTestBase {};

TEST_F(PoolUnitByHeaderTest, CreatePopPushAndDestroyRespectCapacity) {
    std::atomic<int> created(0);
    std::atomic<int> destroyed(0);

    Pool pool(
        [&created]() -> void * {
            created.fetch_add(1, std::memory_order_relaxed);
            return new int(7);
        },
        [&destroyed](void *ptr) {
            destroyed.fetch_add(1, std::memory_order_relaxed);
            delete static_cast<int *>(ptr);
        },
        1);

    void *first = pool.pop();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(*static_cast<int *>(first), 7);
    EXPECT_EQ(created.load(std::memory_order_relaxed), 1);

    pool.push(first);
    EXPECT_EQ(pool.size(), 1u);

    int *overflow = new int(9);
    pool.push(overflow);
    EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 1);

    pool.clear();
    EXPECT_EQ(pool.size(), 0u);
    EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 2);
}

TEST_F(PoolUnitByHeaderTest, PoolGuardAutomaticallyReturnsObject) {
    Pool pool([]() -> void * { return new int(42); },
              [](void *ptr) { delete static_cast<int *>(ptr); }, 8);

    {
        PoolGuard<int> guard(pool);
        ASSERT_TRUE(static_cast<bool>(guard));
        EXPECT_EQ(*guard, 42);
    }

    EXPECT_EQ(pool.size(), 1u);
}

TEST_F(PoolUnitByHeaderTest, PopWithoutCreateAndPushNullAreNoop) {
    Pool pool(nullptr, nullptr, 4);

    EXPECT_EQ(pool.pop(), nullptr);
    pool.push(nullptr);
    EXPECT_EQ(pool.size(), 0u);
}

TEST_F(PoolUnitByHeaderTest, BucketsAreThreadLocalAndClearReleasesAll) {
    std::atomic<int> destroyed(0);
    Pool pool(
        nullptr,
        [&destroyed](void *ptr) {
            destroyed.fetch_add(1, std::memory_order_relaxed);
            delete static_cast<int *>(ptr);
        },
        8);

    std::thread worker([&pool]() {
        pool.push(new int(11));
        EXPECT_EQ(pool.size(), 1u);
    });
    worker.join();

    // 主线程访问的是另一个分桶，因此看不到子线程缓存对象。
    EXPECT_EQ(pool.size(), 0u);

    pool.clear();
    EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 1);
}

TEST_F(PoolUnitByHeaderTest, PopReturnsCachedElementFromSameThreadBucket) {
    Pool pool(nullptr, nullptr, 8);

    int *value = new int(123);
    pool.push(value);
    EXPECT_EQ(pool.size(), 1u);

    void *out = pool.pop();
    EXPECT_EQ(out, value);
    EXPECT_EQ(pool.size(), 0u);
    delete static_cast<int *>(out);
}

TEST_F(PoolUnitByHeaderTest, CopyAndMoveConstructorsKeepUsableState) {
    std::atomic<int> destroyed(0);
    Pool original([]() -> void * { return new int(7); },
                  [&destroyed](void *ptr) {
                      destroyed.fetch_add(1, std::memory_order_relaxed);
                      delete static_cast<int *>(ptr);
                  },
                  2);

    Pool copied(original);
    void *copied_elem = copied.pop();
    ASSERT_NE(copied_elem, nullptr);
    copied.push(copied_elem);

    Pool moved(std::move(original));
    void *moved_elem = moved.pop();
    ASSERT_NE(moved_elem, nullptr);
    moved.push(moved_elem);

    moved.clear();
    copied.clear();
    EXPECT_GE(destroyed.load(std::memory_order_relaxed), 1);
}

TEST_F(PoolUnitByHeaderTest, MovedFromPoolMethodsAreNoop) {
    Pool source(nullptr, nullptr, 4);
    Pool moved(std::move(source));
    (void)moved;

    EXPECT_EQ(source.pop(), nullptr);
    int *tmp = new int(9);
    source.push(tmp);
    source.clear();
    EXPECT_EQ(source.size(), 0u);
    delete tmp;
}

TEST_F(PoolUnitByHeaderTest, UnlimitedCapacityNeverDestroysOnPush) {
    std::atomic<int> destroyed(0);
    Pool pool(
        nullptr,
        [&destroyed](void *ptr) {
            destroyed.fetch_add(1, std::memory_order_relaxed);
            delete static_cast<int *>(ptr);
        },
        static_cast<size_t>(-1));

    pool.push(new int(1));
    pool.push(new int(2));
    pool.push(new int(3));
    EXPECT_EQ(pool.size(), 3u);
    EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 0);

    pool.clear();
    EXPECT_EQ(destroyed.load(std::memory_order_relaxed), 3);
}

TEST_F(PoolUnitByHeaderTest, ClearWithoutDestroyCallbackDropsElementsSafely) {
    Pool pool(nullptr, nullptr, 2);
    int a = 7;
    int b = 8;
    pool.push(&a);
    pool.push(&b);
    EXPECT_EQ(pool.size(), 2u);

    pool.clear();
    EXPECT_EQ(pool.size(), 0u);
}

TEST_F(PoolUnitByHeaderTest,
       CrossThreadBucketCannotBePoppedFromMainThreadWithoutCreate) {
    Pool pool(nullptr, nullptr, 4);
    static int value = 99;

    std::thread worker([&pool]() { pool.push(&value); });
    worker.join();

    // Main thread has different bucket and no create callback.
    EXPECT_EQ(pool.pop(), nullptr);
}

} // namespace
} // namespace zco
