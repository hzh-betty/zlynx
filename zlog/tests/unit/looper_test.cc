#include "looper.h"
#include <atomic>
#include <chrono>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thread>

using namespace zlog;

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
      [&](Buffer &buf) {
        receivedData = std::string(buf.begin(), buf.readAbleSize());
        callbackInvoked = true;
      },
      AsyncType::ASYNC_SAFE, std::chrono::milliseconds(50));

  looper.push("hello", 5);

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  looper.stop();

  EXPECT_TRUE(callbackInvoked);
  EXPECT_EQ(receivedData, "hello");
}

TEST_F(LooperTest, MultiplePushes) {
  std::vector<std::string> received;
  std::mutex mtx;

  AsyncLooper looper(
      [&](Buffer &buf) {
        std::lock_guard<std::mutex> lock(mtx);
        received.push_back(std::string(buf.begin(), buf.readAbleSize()));
      },
      AsyncType::ASYNC_UNSAFE, std::chrono::milliseconds(50));

  for (int i = 0; i < 10; i++) {
    std::string msg = "msg" + std::to_string(i) + "\n";
    looper.push(msg.c_str(), msg.size());
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  looper.stop();

  std::string allReceived;
  for (size_t i = 0; i < received.size(); ++i) {
    allReceived += received[i];
  }

  for (int i = 0; i < 10; i++) {
    std::string expected = "msg" + std::to_string(i) + "\n";
    EXPECT_THAT(allReceived, ::testing::HasSubstr(expected));
  }
}

TEST_F(LooperTest, SafeModeBlocking) {
  std::atomic<int> pushCount(0);

  AsyncLooper looper(
      [&](Buffer &buf) {
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

  AsyncLooper looper([&](Buffer &buf) { totalReceived += buf.readAbleSize(); },
                     AsyncType::ASYNC_UNSAFE, std::chrono::milliseconds(50));

  for (int i = 0; i < 100; i++) {
    std::string msg = "test_message_" + std::to_string(i) + "\n";
    looper.push(msg.c_str(), msg.size());
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  looper.stop();

  EXPECT_GT(totalReceived.load(), static_cast<size_t>(0));
}

TEST_F(LooperTest, StopWithPendingData) {
  std::atomic<bool> callbackCalled(false);

  AsyncLooper looper([&](Buffer &buf) { callbackCalled = true; },
                     AsyncType::ASYNC_SAFE, std::chrono::milliseconds(1000));

  looper.push("data", 4);
  looper.stop();

  SUCCEED();
}

TEST_F(LooperTest, EmptyBuffer) {
  std::atomic<int> count(0);

  AsyncLooper looper([&](Buffer &buf) { count++; }, AsyncType::ASYNC_SAFE,
                     std::chrono::milliseconds(50));

  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  looper.stop();

  SUCCEED();
}

TEST_F(LooperTest, LargeDataPush) {
  std::atomic<size_t> receivedBytes(0);

  AsyncLooper looper([&](Buffer &buf) { receivedBytes += buf.readAbleSize(); },
                     AsyncType::ASYNC_UNSAFE, std::chrono::milliseconds(100));

  std::string largeData(1024 * 1024, 'A'); // 1MB
  looper.push(largeData.c_str(), largeData.size());

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  looper.stop();

  EXPECT_EQ(receivedBytes.load(), largeData.size());
}

TEST_F(LooperTest, ConcurrentPushes) {
  std::atomic<size_t> totalReceived(0);

  AsyncLooper looper([&](Buffer &buf) { totalReceived += buf.readAbleSize(); },
                     AsyncType::ASYNC_UNSAFE, std::chrono::milliseconds(50));

  std::vector<std::thread> threads;
  for (int t = 0; t < 4; t++) {
    threads.push_back(std::thread([&looper, t]() {
      for (int i = 0; i < 100; i++) {
        std::string msg =
            "thread" + std::to_string(t) + "_msg" + std::to_string(i) + "\n";
        looper.push(msg.c_str(), msg.size());
      }
    }));
  }

  for (size_t i = 0; i < threads.size(); ++i) {
    threads[i].join();
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  looper.stop();

  EXPECT_GT(totalReceived.load(), static_cast<size_t>(0));
}

TEST_F(LooperTest, FlushOnThreshold) {
  std::atomic<int> flushCount(0);

  AsyncLooper looper([&](Buffer &buf) { flushCount++; },
                     AsyncType::ASYNC_UNSAFE, std::chrono::milliseconds(5000));

  std::string chunk(1024 * 100, 'X'); // 100KB
  for (int i = 0; i < 20; i++) {
    looper.push(chunk.c_str(), chunk.size());
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  looper.stop();

  EXPECT_GE(flushCount.load(), 1);
}

TEST_F(LooperTest, CallbackException) {
  std::atomic<int> count(0);

  AsyncLooper looper(
      [&](Buffer &buf) {
        count++;
        if (count == 1) {
          throw std::runtime_error("Test exception");
        }
      },
      AsyncType::ASYNC_UNSAFE, std::chrono::milliseconds(50));

  looper.push("data1", 5);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  looper.push("data2", 5);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  looper.stop();

  EXPECT_GE(count.load(), 1);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
