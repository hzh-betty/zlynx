#include "zlog/logger.h"
#include "async/looper.h"
#include <atomic>
#include <chrono>
#include <future>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thread>

using namespace zlog;
using namespace zlog::detail;

class LooperTest : public ::testing::Test {
  protected:
    void SetUp() override {
        callbackCount.store(0);
        totalBytesReceived.store(0);
    }

    std::atomic<int> callbackCount;
    std::atomic<size_t> totalBytesReceived;

    LooperTest() : callbackCount(0), totalBytesReceived(0) {}
};

TEST_F(LooperTest, BasicPushAndCallback) {
    bool callbackInvoked = false;
    std::string receivedData;

    AsyncLooper looper(
        [&](const char *data, size_t len) {
            receivedData = std::string(data, len);
            callbackInvoked = true;
        },
        AsyncType::ASYNC_SAFE, std::chrono::milliseconds(50));

    looper.push("hello", 5);

    looper.flush();
    looper.stop();

    EXPECT_TRUE(callbackInvoked);
    EXPECT_EQ(receivedData, "hello");
}

TEST_F(LooperTest, MultiplePushes) {
    std::vector<std::string> received;
    std::mutex mtx;

    AsyncLooper looper(
        [&](const char *data, size_t len) {
            std::lock_guard<std::mutex> lock(mtx);
            received.push_back(std::string(data, len));
        },
        AsyncType::ASYNC_UNSAFE, std::chrono::milliseconds(50));

    for (int i = 0; i < 10; i++) {
        std::string msg = "msg" + std::to_string(i) + "\n";
        looper.push(msg.c_str(), msg.size());
    }

    looper.flush();
    looper.stop();

    ASSERT_EQ(received.size(), 10u);
    for (int i = 0; i < 10; i++) {
        std::string expected = "msg" + std::to_string(i) + "\n";
        EXPECT_EQ(received[i], expected);
    }
}

TEST_F(LooperTest, SafeModeBlocking) {
    std::atomic<int> pushCount(0);

    AsyncLooper looper(
        [&](const char *data, size_t len) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        },
        AsyncType::ASYNC_SAFE, std::chrono::milliseconds(100));

    for (int i = 0; i < 5; i++) {
        std::string msg(100, 'x');
        looper.push(msg.c_str(), msg.size());
        pushCount++;
    }

    EXPECT_EQ(pushCount.load(), 5);
    looper.stop();
}

TEST_F(LooperTest, UnsafeModeNonBlocking) {
    std::atomic<size_t> totalReceived(0);

    AsyncLooper looper(
        [&](const char *data, size_t len) { totalReceived += len; },
        AsyncType::ASYNC_UNSAFE, std::chrono::milliseconds(50));

    size_t expectedBytes = 0;
    for (int i = 0; i < 100; i++) {
        std::string msg = "test_message_" + std::to_string(i) + "\n";
        looper.push(msg.c_str(), msg.size());
        expectedBytes += msg.size();
    }

    looper.flush();
    looper.stop();

    EXPECT_EQ(totalReceived.load(), expectedBytes);
}

TEST_F(LooperTest, StopWithPendingData) {
    std::atomic<bool> callbackCalled(false);

    AsyncLooper looper([&](const char *data, size_t len) { callbackCalled = true; },
                       AsyncType::ASYNC_SAFE, std::chrono::milliseconds(1000));

    looper.push("data", 4);
    looper.stop();

    EXPECT_TRUE(callbackCalled.load());
}

TEST_F(LooperTest, EmptyBuffer) {
    std::atomic<int> count(0);

    AsyncLooper looper([&](const char *data, size_t len) { count++; }, AsyncType::ASYNC_SAFE,
                       std::chrono::milliseconds(50));

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    looper.stop();

    EXPECT_EQ(count.load(), 0);
}

TEST_F(LooperTest, LargeDataPush) {
    std::atomic<size_t> receivedBytes(0);

    AsyncLooper looper(
        [&](const char *data, size_t len) { receivedBytes += len; },
        AsyncType::ASYNC_UNSAFE, std::chrono::milliseconds(100));

    std::string largeData(1024 * 1024, 'A'); // 1MB
    looper.push(largeData.c_str(), largeData.size());

    looper.flush();
    looper.stop();

    EXPECT_EQ(receivedBytes.load(), largeData.size());
}

TEST_F(LooperTest, ConcurrentPushes) {
    std::atomic<size_t> totalReceived(0);

    AsyncLooper looper(
        [&](const char *data, size_t len) { totalReceived += len; },
        AsyncType::ASYNC_UNSAFE, std::chrono::milliseconds(50));

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
        threads.push_back(std::thread([&looper, t]() {
            for (int i = 0; i < 100; i++) {
                std::string msg = "thread" + std::to_string(t) + "_msg" +
                                  std::to_string(i) + "\n";
                looper.push(msg.c_str(), msg.size());
            }
        }));
    }

    for (size_t i = 0; i < threads.size(); ++i) {
        threads[i].join();
    }

    looper.flush();
    looper.stop();

    size_t expectedBytes = 0;
    for (int t = 0; t < 4; ++t) {
        for (int i = 0; i < 100; ++i) {
            expectedBytes += ("thread" + std::to_string(t) + "_msg" +
                              std::to_string(i) + "\n").size();
        }
    }
    EXPECT_EQ(totalReceived.load(), expectedBytes);
}

TEST_F(LooperTest, FlushOnThreshold) {
    std::atomic<int> flushCount(0);
    std::promise<void> firstCallback;
    const auto started = firstCallback.get_future();

    AsyncLooper looper([&](const char *, size_t) {
                           if (flushCount.fetch_add(1) == 0) firstCallback.set_value();
                       },
                       AsyncType::ASYNC_UNSAFE,
                       std::chrono::milliseconds(5000));

    std::string chunk(1024 * 100, 'X'); // 100KB
    for (int i = 0; i < 20; i++) {
        looper.push(chunk.c_str(), chunk.size());
    }

    // 在最大等待时间和 stop 之前验证阈值会主动唤醒消费者。
    EXPECT_EQ(started.wait_for(std::chrono::milliseconds(500)), std::future_status::ready);
    looper.stop();

    EXPECT_EQ(flushCount.load(), 20);
}

TEST_F(LooperTest, CallbackException) {
    std::atomic<int> count(0);

    AsyncLooper looper(
        [&](const char *data, size_t len) {
            count++;
            if (count == 1) {
                throw std::runtime_error("Test exception");
            }
        },
        AsyncType::ASYNC_UNSAFE, std::chrono::milliseconds(50));

    looper.push("data1", 5);
    looper.push("data2", 5);
    EXPECT_THROW(looper.flush(), std::runtime_error);
    EXPECT_THROW(looper.stop(), std::runtime_error);

    EXPECT_EQ(count.load(), 2);
}

TEST_F(LooperTest, CallbackUnknownException) {
    std::atomic<int> count(0);

    AsyncLooper looper(
        [&](const char *data, size_t len) {
            (void)data;
            (void)len;
            count++;
            if (count == 1) {
                throw 42;
            }
        },
        AsyncType::ASYNC_UNSAFE, std::chrono::milliseconds(20));

    looper.push("a", 1);
    looper.push("b", 1);
    EXPECT_ANY_THROW(looper.flush());
    EXPECT_ANY_THROW(looper.stop());
    EXPECT_EQ(count.load(), 2);
}

TEST_F(LooperTest, OversizedMessagesAreRejectedBeforeWaiting) {
    for (AsyncType mode : {AsyncType::ASYNC_SAFE, AsyncType::ASYNC_UNSAFE}) {
        AsyncLooper looper([](const char *, size_t) {}, mode, std::chrono::milliseconds(1));
        const size_t limit = mode == AsyncType::ASYNC_SAFE
                                 ? kDefaultBufferSize : kMaxBufferSize;
        EXPECT_THROW(looper.push("x", limit + 1), std::length_error);
        looper.push("ok", 2);
        looper.stop();
        EXPECT_THROW(looper.push("x", 1), std::runtime_error);
    }
}

TEST_F(LooperTest, StopWakesAllBlockedProducersAndDrainsAcceptedData) {
    std::mutex gate;
    std::condition_variable cv;
    bool consuming = false, release = false;
    size_t received = 0;
    AsyncLooper looper([&](const char *data, size_t len) {
        std::unique_lock<std::mutex> lock(gate);
        consuming = true;
        cv.notify_all();
        cv.wait(lock, [&]() { return release; });
        received += len;
    }, AsyncType::ASYNC_SAFE, std::chrono::milliseconds(1));
    const std::string full(kDefaultBufferSize - kRecordHeaderSize, 'x');
    looper.push(full.data(), full.size());
    {
        std::unique_lock<std::mutex> lock(gate);
        cv.wait(lock, [&]() { return consuming; });
    }
    looper.push(full.data(), full.size());
    std::atomic<int> rejected(0);
    std::vector<std::thread> producers;
    for (int i = 0; i < 3; ++i) {
        producers.emplace_back([&]() {
            try { looper.push("x", 1); }
            catch (const std::runtime_error &) { ++rejected; }
        });
    }
    std::thread stopper([&]() { looper.stop(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (rejected.load() != 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(rejected.load(), 3);
    {
        std::lock_guard<std::mutex> lock(gate);
        release = true;
    }
    cv.notify_all();
    for (auto &producer : producers) producer.join();
    stopper.join();
    EXPECT_EQ(received, 2 * full.size());
}

TEST_F(LooperTest, ConcurrentStopIsIdempotent) {
    AsyncLooper looper([](const char *, size_t) {}, AsyncType::ASYNC_SAFE,
                       std::chrono::milliseconds(1));
    looper.push("x", 1);
    std::thread first([&]() { looper.stop(); });
    std::thread second([&]() { looper.stop(); });
    first.join();
    second.join();
    EXPECT_NO_THROW(looper.stop());
}

TEST_F(LooperTest, FlushDrainsBelowThresholdAndIncludesSinkFlush) {
    std::vector<std::string> records;
    int flushes = 0;
    AsyncLooper looper([&](const char *data, size_t len) { records.emplace_back(data, len); },
        AsyncType::ASYNC_SAFE, std::chrono::hours(1), [&] { ++flushes; });
    looper.push("first", 5);
    looper.push("", 0);
    looper.flush();
    EXPECT_EQ(records, (std::vector<std::string>{"first", ""}));
    EXPECT_EQ(flushes, 1);
    looper.push("last", 4);
    looper.flush();
    EXPECT_EQ(records.back(), "last");
    EXPECT_EQ(flushes, 2);
    looper.stop();
    EXPECT_EQ(flushes, 3);
}

TEST_F(LooperTest, FramedCapacityBoundaryAndInvalidWaitTimesAreRejected) {
    for (AsyncType mode : {AsyncType::ASYNC_SAFE, AsyncType::ASYNC_UNSAFE}) {
        AsyncLooper looper([](const char *, size_t) {}, mode, std::chrono::hours(1));
        const size_t limit = mode == AsyncType::ASYNC_SAFE ? kDefaultBufferSize : kMaxBufferSize;
        EXPECT_THROW(looper.push("x", limit), std::length_error);
        EXPECT_THROW(looper.push("x", limit - kRecordHeaderSize + 1), std::length_error);
        looper.stop();
    }
    size_t received = 0;
    AsyncLooper looper([&](const char *, size_t len) { received += len; },
                       AsyncType::ASYNC_SAFE, std::chrono::hours(1));
    const std::string full(kDefaultBufferSize - kRecordHeaderSize, 'x');
    looper.push(full.data(), full.size());
    looper.flush();
    EXPECT_EQ(received, full.size());
    EXPECT_THROW(AsyncLooper([](const char *, size_t) {}, AsyncType::ASYNC_SAFE,
                            std::chrono::milliseconds(0)), std::invalid_argument);
    EXPECT_THROW(AsyncLooper([](const char *, size_t) {}, AsyncType::ASYNC_SAFE,
                            std::chrono::milliseconds(-1)), std::invalid_argument);
}

TEST_F(LooperTest, ConcurrentFlushAndStopCompleteWithoutLosingRecords) {
    std::atomic<size_t> bytes(0);
    AsyncLooper looper([&](const char *, size_t len) { bytes += len; },
                       AsyncType::ASYNC_SAFE, std::chrono::hours(1));
    for (int i = 0; i < 1000; ++i) looper.push("record", 6);
    std::thread first([&] { looper.flush(); });
    std::thread second([&] { looper.stop(); });
    first.join(); second.join();
    EXPECT_EQ(bytes.load(), 6000u);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
