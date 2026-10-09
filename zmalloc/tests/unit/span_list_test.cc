#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <type_traits>
#include <vector>

#include "zmalloc/internal/span_list.h"

// 内嵌哨兵地址被普通节点引用，编译期禁止使地址失效的复制和移动。
static_assert(!std::is_copy_constructible<zmalloc::SpanList>::value,
              "SpanList must not be copied");
static_assert(!std::is_copy_assignable<zmalloc::SpanList>::value,
              "SpanList must not be copy assigned");
static_assert(!std::is_move_constructible<zmalloc::SpanList>::value,
              "SpanList must not be moved");
static_assert(!std::is_move_assignable<zmalloc::SpanList>::value,
              "SpanList must not be move assigned");

TEST(SpanListTest, IndependentSentinelsAndEmptyLinks) {
    zmalloc::SpanList first;
    zmalloc::SpanList second;
    EXPECT_NE(first.end(), second.end());
    EXPECT_TRUE(first.empty());
    EXPECT_EQ(first.begin(), first.end());
    EXPECT_EQ(first.end()->prev, first.end());
    EXPECT_EQ(second.end()->next, second.end());
}

TEST(SpanListTest, InsertEraseAndPopPreserveCircularLinks) {
    zmalloc::SpanList list;
    zmalloc::Span first;
    zmalloc::Span second;
    list.push_front(&first);
    list.insert(list.end(), &second);
    EXPECT_EQ(list.begin(), &first);
    EXPECT_EQ(first.next, &second);
    EXPECT_EQ(second.prev, &first);
    EXPECT_EQ(second.next, list.end());
    EXPECT_EQ(list.end()->prev, &second);
    list.erase(&first);
    EXPECT_EQ(list.begin(), &second);
    EXPECT_EQ(second.prev, list.end());
    EXPECT_EQ(list.pop_front(), &second);
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.end()->prev, list.end());
}

TEST(SpanListTest, ConcurrentConstructionUsesIndependentStorage) {
    std::atomic<bool> go(false);
    std::atomic<bool> valid(true);
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) {
        workers.emplace_back([&] {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            // 反复并发构造，覆盖旧实现中共享无锁哨兵池的切分竞争。
            for (int round = 0; round < 1000; ++round) {
                zmalloc::SpanList list;
                zmalloc::Span span;
                list.push_front(&span);
                if (list.begin() != &span || span.next != list.end() ||
                    span.prev != list.end() || list.pop_front() != &span ||
                    !list.empty() || list.end()->prev != list.end()) {
                    valid.store(false, std::memory_order_relaxed);
                }
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto &worker : workers) {
        worker.join();
    }
    EXPECT_TRUE(valid.load(std::memory_order_relaxed));
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
