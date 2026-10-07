/**
 * @file free_list_test.cc
 * @brief FreeList 单元测试
 */

#include "zmalloc/internal/free_list.h"
#include <gtest/gtest.h>

namespace zmalloc {
namespace {

class FreeListTest : public ::testing::Test {
  protected:
    FreeList list_;
    // 用于测试的内存块
    char blocks_[10][sizeof(void *)];

    void SetUp() override {
        // 初始化
        for (int i = 0; i < 10; ++i) {
            next_obj(blocks_[i]) = nullptr;
        }
    }
};
class FreeListCountParamTest
    : public FreeListTest, public ::testing::WithParamInterface<int> {};

static size_t ChainLength(void *start, size_t hard_limit = 1024) {
    size_t n = 0;
    void *cur = start;
    while (cur != nullptr && n < hard_limit) {
        cur = next_obj(cur);
        ++n;
    }
    return n;
}

static void *NthNode(void *start, size_t n) {
    void *cur = start;
    while (cur != nullptr && n > 0) {
        cur = next_obj(cur);
        --n;
    }
    return cur;
}

TEST_F(FreeListTest, InitiallyEmpty) {
    EXPECT_TRUE(list_.empty());
    EXPECT_EQ(list_.size(), 0);
}

TEST_F(FreeListTest, PushAndPop) {
    list_.push(blocks_[0]);
    EXPECT_FALSE(list_.empty());
    EXPECT_EQ(list_.size(), 1);

    void *obj = list_.pop();
    EXPECT_EQ(obj, blocks_[0]);
    EXPECT_TRUE(list_.empty());
    EXPECT_EQ(list_.size(), 0);
}

TEST_F(FreeListTest, MultiplePushPop) {
    list_.push(blocks_[0]);
    list_.push(blocks_[1]);
    list_.push(blocks_[2]);
    EXPECT_EQ(list_.size(), 3);

    // LIFO 顺序
    EXPECT_EQ(list_.pop(), blocks_[2]);
    EXPECT_EQ(list_.pop(), blocks_[1]);
    EXPECT_EQ(list_.pop(), blocks_[0]);
    EXPECT_TRUE(list_.empty());
}

TEST_F(FreeListTest, PushRange) {
    // 构建一个链表
    next_obj(blocks_[0]) = blocks_[1];
    next_obj(blocks_[1]) = blocks_[2];
    next_obj(blocks_[2]) = nullptr;

    list_.push_range(blocks_[0], blocks_[2], 3);
    EXPECT_EQ(list_.size(), 3);
    EXPECT_FALSE(list_.empty());
}

TEST_F(FreeListTest, PopBatch) {
    list_.push(blocks_[0]);
    list_.push(blocks_[1]);
    list_.push(blocks_[2]);
    list_.push(blocks_[3]);
    list_.push(blocks_[4]);

    void *start = nullptr;
    void *end = nullptr;
    void *batch[10] = {};
    ASSERT_EQ(list_.pop_batch(batch, 3), static_cast<size_t>(3));
    start = batch[0];
    end = batch[(3) - 1];

    EXPECT_EQ(list_.size(), 2);
    EXPECT_NE(start, nullptr);
    EXPECT_NE(end, nullptr);
}

TEST_F(FreeListTest, MaxSize) {
    EXPECT_EQ(list_.max_size(), 1);
    list_.max_size() = 10;
    EXPECT_EQ(list_.max_size(), 10);
}

// 边界条件：只有一个元素
TEST_F(FreeListTest, PopBatchSingleElement) {
    list_.push(blocks_[0]);
    void *start = nullptr;
    void *end = nullptr;
    void *batch[10] = {};
    ASSERT_EQ(list_.pop_batch(batch, 1), static_cast<size_t>(1));
    start = batch[0];
    end = batch[(1) - 1];
    EXPECT_EQ(start, blocks_[0]);
    EXPECT_EQ(end, blocks_[0]);
    EXPECT_EQ(list_.size(), 0);
}

// 边界条件：pop_batch 请求恰好等于可用数量
TEST_F(FreeListTest, PopBatchExactCount) {
    list_.push(blocks_[0]);
    list_.push(blocks_[1]);
    void *start = nullptr;
    void *end = nullptr;
    void *batch[10] = {};
    ASSERT_EQ(list_.pop_batch(batch, 2), static_cast<size_t>(2));
    start = batch[0];
    end = batch[(2) - 1];
    // 取出全部
    EXPECT_EQ(list_.size(), 0);
    EXPECT_NE(start, nullptr);
    EXPECT_NE(end, nullptr);
}

// 大量 Push 测试
TEST_F(FreeListTest, ManyPush) {
    char large_blocks[100][sizeof(void *)];
    for (int i = 0; i < 100; ++i) {
        next_obj(large_blocks[i]) = nullptr;
        list_.push(large_blocks[i]);
    }
    EXPECT_EQ(list_.size(), 100);
    for (int i = 0; i < 100; ++i) {
        EXPECT_NE(list_.pop(), nullptr);
    }
    EXPECT_TRUE(list_.empty());
}

// Push/Pop 交替
TEST_F(FreeListTest, AlternatePushPop) {
    for (int i = 0; i < 50; ++i) {
        list_.push(blocks_[0]);
        EXPECT_EQ(list_.pop(), blocks_[0]);
    }
    EXPECT_TRUE(list_.empty());
}

// 验证 LIFO 顺序
TEST_F(FreeListTest, LIFOOrder) {
    for (int i = 0; i < 10; ++i) {
        list_.push(blocks_[i]);
    }
    for (int i = 9; i >= 0; --i) {
        EXPECT_EQ(list_.pop(), blocks_[i]);
    }
}

// push_range 后验证大小
TEST_F(FreeListTest, PushRangeSize) {
    next_obj(blocks_[0]) = blocks_[1];
    next_obj(blocks_[1]) = blocks_[2];
    next_obj(blocks_[2]) = blocks_[3];
    next_obj(blocks_[3]) = blocks_[4];
    next_obj(blocks_[4]) = nullptr;

    list_.push_range(blocks_[0], blocks_[4], 5);
    EXPECT_EQ(list_.size(), 5);
}

// empty() 和 size() 一致性
TEST_F(FreeListTest, EmptySizeConsistency) {
    EXPECT_TRUE(list_.empty());
    EXPECT_EQ(list_.size(), 0);

    list_.push(blocks_[0]);
    EXPECT_FALSE(list_.empty());
    EXPECT_EQ(list_.size(), 1);

    list_.pop();
    EXPECT_TRUE(list_.empty());
    EXPECT_EQ(list_.size(), 0);
}

// max_size 修改测试
TEST_F(FreeListTest, MaxSizeModification) {
    list_.max_size() = 5;
    EXPECT_EQ(list_.max_size(), 5);
    list_.max_size() += 3;
    EXPECT_EQ(list_.max_size(), 8);
}

// 连续 push_range
TEST_F(FreeListTest, MultiplePushRange) {
    next_obj(blocks_[0]) = blocks_[1];
    next_obj(blocks_[1]) = nullptr;
    list_.push_range(blocks_[0], blocks_[1], 2);

    next_obj(blocks_[2]) = blocks_[3];
    next_obj(blocks_[3]) = nullptr;
    list_.push_range(blocks_[2], blocks_[3], 2);

    EXPECT_EQ(list_.size(), 4);
}

// pop_batch 验证链表结构
TEST_F(FreeListTest, PopBatchChainStructure) {
    for (int i = 0; i < 5; ++i) {
        list_.push(blocks_[i]);
    }
    void *start = nullptr;
    void *end = nullptr;
    void *batch[10] = {};
    ASSERT_EQ(list_.pop_batch(batch, 3), static_cast<size_t>(3));
    start = batch[0];
    end = batch[(3) - 1];

    // 验证取出的是一个链表
    EXPECT_NE(start, nullptr);
    EXPECT_NE(end, nullptr);
    // 验证剩余
    EXPECT_EQ(list_.size(), 2);
}


TEST_F(FreeListTest, PushRangeAndPopAll) {
    next_obj(blocks_[0]) = blocks_[1];
    next_obj(blocks_[1]) = blocks_[2];
    next_obj(blocks_[2]) = nullptr;

    list_.push_range(blocks_[0], blocks_[2], 3);
    EXPECT_EQ(list_.size(), 3u);

    void *start = nullptr;
    void *end = nullptr;
    void *batch[10] = {};
    ASSERT_EQ(list_.pop_batch(batch, 3), static_cast<size_t>(3));
    start = batch[0];
    end = batch[(3) - 1];
    EXPECT_EQ(start, blocks_[0]);
    EXPECT_EQ(end, blocks_[2]);
    EXPECT_EQ(next_obj(end), nullptr);
    EXPECT_TRUE(list_.empty());
}

TEST_F(FreeListTest, PushThenPushRangeMaintainsSize) {
    list_.push(blocks_[0]);

    next_obj(blocks_[1]) = blocks_[2];
    next_obj(blocks_[2]) = nullptr;
    list_.push_range(blocks_[1], blocks_[2], 2);
    EXPECT_EQ(list_.size(), 3u);
}

TEST_F(FreeListTest, PopBatchOneElementMatchesPop) {
    list_.push(blocks_[0]);
    void *start = nullptr;
    void *end = nullptr;
    void *batch[10] = {};
    ASSERT_EQ(list_.pop_batch(batch, 1), static_cast<size_t>(1));
    start = batch[0];
    end = batch[(1) - 1];
    EXPECT_EQ(start, blocks_[0]);
    EXPECT_EQ(end, blocks_[0]);
    EXPECT_TRUE(list_.empty());
    EXPECT_EQ(list_.size(), 0u);
}

TEST_P(FreeListCountParamTest, PopBatchReturnsRequestedChainLength) {
    const int n = GetParam();
    for (int i = 0; i < 10; ++i) {
        list_.push(blocks_[i]);
    }
    void *start = nullptr;
    void *end = nullptr;
    void *batch[10] = {};
    ASSERT_EQ(list_.pop_batch(batch, n), static_cast<size_t>(n));
    start = batch[0];
    end = batch[(n) - 1];
    ASSERT_NE(start, nullptr);
    ASSERT_NE(end, nullptr);
    EXPECT_EQ(ChainLength(start), static_cast<size_t>(n));
    EXPECT_EQ(next_obj(end), nullptr);
    EXPECT_EQ(list_.size(), static_cast<size_t>(10 - n));
    EXPECT_EQ(start, blocks_[9]);
    EXPECT_EQ(end, NthNode(start, static_cast<size_t>(n - 1)));
}

TEST_P(FreeListCountParamTest, PushRangeThenPopPreservesSequentialOrder) {
    const int n = GetParam();
    for (int i = 0; i < n; ++i) {
        next_obj(blocks_[i]) = (i + 1 < n) ? blocks_[i + 1] : nullptr;
    }
    list_.push_range(blocks_[0], blocks_[n - 1], n);
    EXPECT_EQ(list_.size(), static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        EXPECT_EQ(list_.pop(), blocks_[i]);
    }
    EXPECT_TRUE(list_.empty());
}

INSTANTIATE_TEST_SUITE_P(Counts, FreeListCountParamTest,
                         ::testing::Values(1, 2, 3, 4, 5, 6, 7, 8, 9, 10));

// push / push_range 组合：确保计数与弹出顺序一致
TEST_F(FreeListTest, PushThenPushRangeThenPopAllOrder) {
    list_.push(blocks_[9]);
    next_obj(blocks_[0]) = blocks_[1];
    next_obj(blocks_[1]) = blocks_[2];
    next_obj(blocks_[2]) = nullptr;
    list_.push_range(blocks_[0], blocks_[2], 3);
    EXPECT_EQ(list_.size(), 4u);
    EXPECT_EQ(list_.pop(), blocks_[0]);
    EXPECT_EQ(list_.pop(), blocks_[1]);
    EXPECT_EQ(list_.pop(), blocks_[2]);
    EXPECT_EQ(list_.pop(), blocks_[9]);
    EXPECT_TRUE(list_.empty());
}

TEST_F(FreeListTest, PushRangeThenPushThenPopAllOrder) {
    next_obj(blocks_[0]) = blocks_[1];
    next_obj(blocks_[1]) = blocks_[2];
    next_obj(blocks_[2]) = nullptr;
    list_.push_range(blocks_[0], blocks_[2], 3);
    list_.push(blocks_[9]);
    EXPECT_EQ(list_.size(), 4u);
    EXPECT_EQ(list_.pop(), blocks_[9]);
    EXPECT_EQ(list_.pop(), blocks_[0]);
    EXPECT_EQ(list_.pop(), blocks_[1]);
    EXPECT_EQ(list_.pop(), blocks_[2]);
    EXPECT_TRUE(list_.empty());
}

// pop_batch 后再 push_range：确保不会丢链/环链
TEST_F(FreeListTest, PopBatchThenPushRangeRoundTrip) {
    for (int i = 0; i < 10; ++i) {
        list_.push(blocks_[i]);
    }
    void *start = nullptr;
    void *end = nullptr;
    void *batch[10] = {};
    ASSERT_EQ(list_.pop_batch(batch, 5), static_cast<size_t>(5));
    start = batch[0];
    end = batch[(5) - 1];
    ASSERT_EQ(ChainLength(start), 5u);
    list_.push_range(start, end, 5);
    EXPECT_EQ(list_.size(), 10u);
    // 还能完整 pop 出 10 个
    for (int i = 0; i < 10; ++i) {
        EXPECT_NE(list_.pop(), nullptr);
    }
    EXPECT_TRUE(list_.empty());
}


// push_range 后再 pop_batch：验证 end->next 被正确断开
TEST_F(FreeListTest, PushRangeThenPopBatchBreaksEndNext) {
    for (int i = 0; i < 10; ++i) {
        next_obj(blocks_[i]) = (i + 1 < 10) ? blocks_[i + 1] : nullptr;
    }
    list_.push_range(blocks_[0], blocks_[9], 10);
    void *start = nullptr;
    void *end = nullptr;
    void *batch[10] = {};
    ASSERT_EQ(list_.pop_batch(batch, 3), static_cast<size_t>(3));
    start = batch[0];
    end = batch[(3) - 1];
    ASSERT_EQ(ChainLength(start), 3u);
    EXPECT_EQ(next_obj(end), nullptr);
    EXPECT_EQ(list_.size(), 7u);
}

TEST_F(FreeListTest, PushRangeThenPopBatchAll) {
    for (int i = 0; i < 10; ++i) {
        next_obj(blocks_[i]) = (i + 1 < 10) ? blocks_[i + 1] : nullptr;
    }
    list_.push_range(blocks_[0], blocks_[9], 10);
    void *start = nullptr;
    void *end = nullptr;
    void *batch[10] = {};
    ASSERT_EQ(list_.pop_batch(batch, 10), static_cast<size_t>(10));
    start = batch[0];
    end = batch[(10) - 1];
    EXPECT_EQ(ChainLength(start), 10u);
    EXPECT_TRUE(list_.empty());
}

TEST_F(FreeListTest, PopBatchZeroReturnsZeroAndKeepsList) {
    list_.push(blocks_[0]);
    void *batch[1] = {reinterpret_cast<void *>(0x1)};
    EXPECT_EQ(list_.pop_batch(batch, 0), 0u);
    EXPECT_EQ(list_.size(), 1u);
    EXPECT_EQ(list_.pop(), blocks_[0]);
}

TEST_F(FreeListTest, PopBatchExactAllElements) {
    for (int i = 0; i < 5; ++i) {
        list_.push(blocks_[i]);
    }
    void *batch[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    size_t popped = list_.pop_batch(batch, 5);
    ASSERT_EQ(popped, 5u);
    EXPECT_TRUE(list_.empty());
    EXPECT_EQ(batch[0], blocks_[4]);
    EXPECT_EQ(batch[1], blocks_[3]);
    EXPECT_EQ(batch[2], blocks_[2]);
    EXPECT_EQ(batch[3], blocks_[1]);
    EXPECT_EQ(batch[4], blocks_[0]);
    EXPECT_EQ(next_obj(batch[4]), nullptr);
}

TEST_F(FreeListTest, PopBatchPartialKeepsRemainingChain) {
    for (int i = 0; i < 6; ++i) {
        list_.push(blocks_[i]);
    }
    void *batch[3] = {nullptr, nullptr, nullptr};
    size_t popped = list_.pop_batch(batch, 3);
    ASSERT_EQ(popped, 3u);
    EXPECT_EQ(list_.size(), 3u);
    EXPECT_EQ(batch[0], blocks_[5]);
    EXPECT_EQ(batch[1], blocks_[4]);
    EXPECT_EQ(batch[2], blocks_[3]);
    EXPECT_EQ(next_obj(batch[2]), nullptr);
    EXPECT_EQ(list_.pop(), blocks_[2]);
    EXPECT_EQ(list_.pop(), blocks_[1]);
    EXPECT_EQ(list_.pop(), blocks_[0]);
    EXPECT_TRUE(list_.empty());
}

} // namespace
} // namespace zmalloc

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
