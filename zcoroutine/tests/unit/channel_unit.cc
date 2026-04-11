#include <atomic>

#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zcoroutine/channel.h"

namespace zcoroutine {
namespace {

class ChannelUnitByHeaderTest : public test::RuntimeTestBase {};

TEST_F(ChannelUnitByHeaderTest, BasicReadWriteAndCloseSemantics) {
    Channel<int> channel(2, 100);

    EXPECT_TRUE(channel.write(1));
    EXPECT_TRUE(channel.write(2));
    EXPECT_FALSE(channel.write(3, 0));

    int out = 0;
    EXPECT_TRUE(channel.read(out));
    EXPECT_EQ(out, 1);
    EXPECT_TRUE(channel.read(out));
    EXPECT_EQ(out, 2);

    channel.close();
    EXPECT_FALSE(channel.write(9));
    EXPECT_FALSE(channel.read(out, 0));
}

TEST_F(ChannelUnitByHeaderTest, CapacityZeroNormalizesAndDoneTracksLastResult) {
    Channel<int> channel(0, 100);

    EXPECT_TRUE(static_cast<bool>(channel));
    EXPECT_TRUE(channel.try_write(7));
    EXPECT_TRUE(channel.done());

    EXPECT_FALSE(channel.try_write(8));
    EXPECT_FALSE(channel.done());

    int out = 0;
    EXPECT_TRUE(channel.try_read(out));
    EXPECT_EQ(out, 7);
    EXPECT_TRUE(channel.done());

    channel.close();
    EXPECT_FALSE(static_cast<bool>(channel));
}

TEST_F(ChannelUnitByHeaderTest,
       CloseStillAllowsBufferedReadsBeforeFinalFailure) {
    Channel<int> channel(2, 100);

    EXPECT_TRUE(channel.write(10));
    EXPECT_TRUE(channel.write(20));
    channel.close();

    int out = 0;
    EXPECT_TRUE(channel.read(out, 0));
    EXPECT_EQ(out, 10);
    EXPECT_TRUE(channel.read(out, 0));
    EXPECT_EQ(out, 20);

    EXPECT_FALSE(channel.read(out, 0));
    EXPECT_FALSE(channel.done());
}

TEST_F(ChannelUnitByHeaderTest, WrapAroundKeepsFifoOrder) {
    Channel<int> channel(3, 100);

    EXPECT_TRUE(channel.write(1));
    EXPECT_TRUE(channel.write(2));
    EXPECT_TRUE(channel.write(3));

    int out = 0;
    EXPECT_TRUE(channel.read(out, 0));
    EXPECT_EQ(out, 1);
    EXPECT_TRUE(channel.read(out, 0));
    EXPECT_EQ(out, 2);

    EXPECT_TRUE(channel.write(4));
    EXPECT_TRUE(channel.write(5));

    EXPECT_TRUE(channel.read(out, 0));
    EXPECT_EQ(out, 3);
    EXPECT_TRUE(channel.read(out, 0));
    EXPECT_EQ(out, 4);
    EXPECT_TRUE(channel.read(out, 0));
    EXPECT_EQ(out, 5);

    channel.close();
    EXPECT_FALSE(channel.read(out, 0));
}

TEST_F(ChannelUnitByHeaderTest, CoroutineProducerConsumerWorks) {
    init(2);

    Channel<int> channel(8, 500);
    WaitGroup done(2);
    std::atomic<int> sum(0);

    go([&channel, &done]() {
        for (int i = 1; i <= 50; ++i) {
            EXPECT_TRUE(channel.write(i));
        }
        channel.close();
        done.done();
    });

    go([&channel, &done, &sum]() {
        int value = 0;
        while (channel.read(value)) {
            sum.fetch_add(value, std::memory_order_relaxed);
        }
        done.done();
    });

    done.wait();
    EXPECT_EQ(sum.load(std::memory_order_relaxed), 1275);
}

} // namespace
} // namespace zcoroutine
