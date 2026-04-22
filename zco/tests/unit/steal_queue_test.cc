#include <atomic>
#include <deque>

#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zco/internal/steal_queue.h"

namespace zco {
namespace {

class StealQueueUnitTest : public test::RuntimeTestBase {};

void PushNoopTasks(StealQueue *queue, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        queue->push([]() {});
    }
}

TEST_F(StealQueueUnitTest, HandlesNullOutputOrZeroMaxSteal) {
    StealQueue queue;
    PushNoopTasks(&queue, 10);

    EXPECT_EQ(queue.steal(nullptr, 3, 0), 0u);

    std::deque<Task> out;
    EXPECT_EQ(queue.steal(&out, 0, 0), 0u);
    EXPECT_TRUE(out.empty());
}

TEST_F(StealQueueUnitTest, ReturnsZeroWhenTotalNotAboveReserve) {
    StealQueue queue;
    PushNoopTasks(&queue, 5);

    std::deque<Task> out;
    EXPECT_EQ(queue.steal(&out, 8, 5), 0u);
    EXPECT_EQ(queue.steal(&out, 8, 6), 0u);
}

TEST_F(StealQueueUnitTest, UsesPolicyRangesForSmallMediumLargeQueues) {
    {
        StealQueue queue;
        PushNoopTasks(&queue, 5);
        std::deque<Task> out;
        EXPECT_EQ(queue.steal(&out, 100, 0), 1u);
    }

    {
        StealQueue queue;
        PushNoopTasks(&queue, 30);
        std::deque<Task> out;
        EXPECT_EQ(queue.steal(&out, 100, 0), 10u);
    }

    {
        StealQueue queue;
        PushNoopTasks(&queue, 100);
        std::deque<Task> out;
        EXPECT_EQ(queue.steal(&out, 100, 0), 40u);
    }
}

TEST_F(StealQueueUnitTest, AppliesMaxStealAndReserveBoundaries) {
    StealQueue queue;
    PushNoopTasks(&queue, 100);

    std::deque<Task> out;
    EXPECT_EQ(queue.steal(&out, 7, 0), 7u);

    StealQueue queue_with_reserve;
    PushNoopTasks(&queue_with_reserve, 20);
    std::deque<Task> out2;
    EXPECT_EQ(queue_with_reserve.steal(&out2, 50, 19), 1u);
}

TEST_F(StealQueueUnitTest, DrainAndAppendTransferTasksCorrectly) {
    StealQueue source;
    PushNoopTasks(&source, 4);

    std::deque<Task> drained;
    source.drain_all(&drained);
    EXPECT_EQ(source.size(), 0u);
    EXPECT_EQ(drained.size(), 4u);

    StealQueue target;
    target.append(&drained);
    EXPECT_TRUE(drained.empty());
    EXPECT_EQ(target.size(), 4u);

    target.append(nullptr);
    std::deque<Task> empty;
    target.append(&empty);
    EXPECT_EQ(target.size(), 4u);
}

TEST_F(StealQueueUnitTest,
       DrainSomeTransfersBoundedPrefixAndPreservesRemainder) {
    StealQueue queue;
    PushNoopTasks(&queue, 5);

    std::deque<Task> drained;
    queue.drain_some(&drained, 2);
    EXPECT_EQ(drained.size(), 2u);
    EXPECT_EQ(queue.size(), 3u);

    queue.drain_some(&drained, 10);
    EXPECT_EQ(drained.size(), 5u);
    EXPECT_EQ(queue.size(), 0u);

    queue.drain_some(nullptr, 3);
    queue.drain_some(&drained, 0);
    EXPECT_EQ(drained.size(), 5u);
}

} // namespace
} // namespace zco

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
