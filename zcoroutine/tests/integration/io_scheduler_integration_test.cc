/**
 * @file io_scheduler_integration_test.cc
 * @brief IoScheduler集成测试
 * 测试调度器、协程、IO事件、定时器的协同工作
 */

#include "hook.h"
#include "io_scheduler.h"
#include "zcoroutine_logger.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <mutex>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace zcoroutine;

class IoSchedulerIntegrationTest : public ::testing::Test {
protected:
  void SetUp() override { set_hook_enable(true); }

  void TearDown() override { set_hook_enable(false); }
};

// 测试：IoScheduler基本功能
TEST_F(IoSchedulerIntegrationTest, BasicIoScheduler) {
  auto io_scheduler = std::make_shared<IoScheduler>(2, "TestIoScheduler");
  EXPECT_NE(io_scheduler, nullptr);

  std::atomic<int> count{0};

  io_scheduler->start();
  io_scheduler->schedule([&count]() { count++; });

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  io_scheduler->stop();

  EXPECT_GE(count.load(), 1);
}

// 测试：定时器功能
TEST_F(IoSchedulerIntegrationTest, TimerFunction) {
  auto io_scheduler = std::make_shared<IoScheduler>(2, "TimerTest");
  io_scheduler->start();

  std::atomic<int> timer_count{0};

  // 添加单次定时器
  io_scheduler->add_timer(100, [&timer_count]() { timer_count++; });

  // 添加循环定时器
  std::atomic<int> loop_count{0};
  auto timer =
      io_scheduler->add_timer(50, [&loop_count]() { loop_count++; }, true);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  EXPECT_GE(timer_count.load(), 1);
  EXPECT_GE(loop_count.load(), 5); // 500ms / 50ms ≈ 10次

  timer->cancel();
  io_scheduler->stop();
}

// 测试：Pipe IO事件
TEST_F(IoSchedulerIntegrationTest, PipeIoEvent) {
  auto io_scheduler = std::make_shared<IoScheduler>(2, "PipeTest");
  io_scheduler->start();

  int pipe_fds[2];
  ASSERT_EQ(pipe(pipe_fds), 0);

  // 设置非阻塞
  fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
  fcntl(pipe_fds[1], F_SETFL, O_NONBLOCK);

  std::mutex m;
  std::condition_variable cv;
  bool read_done = false;
  bool write_done = false;
  bool write_ok = false;
  std::string received_data;

  // 添加读事件
  io_scheduler->add_event(pipe_fds[0], FdContext::kRead, [&]() {
    char buffer[256];
    ssize_t n = read(pipe_fds[0], buffer, sizeof(buffer) - 1);
    if (n > 0) {
      buffer[n] = '\0';
      {
        std::lock_guard<std::mutex> lk(m);
        received_data = buffer;
        read_done = true;
      }
      cv.notify_one();
    }
  });

  // 延迟写入数据
  io_scheduler->add_timer(100, [&]() {
    const char *msg = "Hello IoScheduler!";
    // 不在异线程/回调中使用 ASSERT/EXPECT，避免 gtest 线程模型导致的非确定行为。
    const ssize_t n = write(pipe_fds[1], msg, strlen(msg));
    {
      std::lock_guard<std::mutex> lk(m);
      write_done = true;
      write_ok = (n == static_cast<ssize_t>(strlen(msg)));
    }
    cv.notify_one();
  });

  // 等待写入与读取完成（避免系统抖动导致固定 sleep 时间不足）
  std::unique_lock<std::mutex> lk(m);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  cv.wait_until(lk, deadline, [&]() { return write_done && read_done; });

  EXPECT_TRUE(write_done);
  EXPECT_TRUE(write_ok);
  EXPECT_TRUE(read_done);
  EXPECT_EQ(received_data, "Hello IoScheduler!");

  close(pipe_fds[0]);
  close(pipe_fds[1]);
  io_scheduler->stop();
}

// 测试：协程与IO事件结合
TEST_F(IoSchedulerIntegrationTest, FiberWithIoEvent) {
  auto io_scheduler = std::make_shared<IoScheduler>(2, "FiberIoTest");
  io_scheduler->start();

  int pipe_fds[2];
  ASSERT_EQ(pipe(pipe_fds), 0);
  fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
  fcntl(pipe_fds[1], F_SETFL, O_NONBLOCK);

  std::atomic<int> step{0};

  // 写协程
  io_scheduler->schedule([&]() {
    step = 1;
    const char *msg = "fiber message";
    ASSERT_EQ(write(pipe_fds[1], msg, strlen(msg)), strlen(msg));
    step = 2;
  });

  // 读协程
  io_scheduler->schedule([&]() {
    io_scheduler->add_event(pipe_fds[0], FdContext::kRead, [&]() {
      char buffer[256];
      ssize_t n = read(pipe_fds[0], buffer, sizeof(buffer) - 1);
      if (n > 0) {
        buffer[n] = '\0';
        EXPECT_EQ(strcmp(buffer, "fiber message"), 0);
        step = 3;
      }
    });
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  EXPECT_GE(step.load(), 2);

  close(pipe_fds[0]);
  close(pipe_fds[1]);
  io_scheduler->stop();
}

// 测试：多个协程并发IO
TEST_F(IoSchedulerIntegrationTest, MultipleCoroutinesIo) {
  auto io_scheduler = std::make_shared<IoScheduler>(4, "MultiIoTest");
  io_scheduler->start();

  const int fiber_count = 10;
  std::atomic<int> completed{0};

  for (int i = 0; i < fiber_count; ++i) {
    io_scheduler->schedule([&, i]() {
      // 模拟IO操作
      io_scheduler->add_timer(50 + i * 10, [&]() { completed++; });
    });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  EXPECT_EQ(completed.load(), fiber_count);
  io_scheduler->stop();
}

TEST_F(IoSchedulerIntegrationTest, HookSystemCall) {
  auto io_scheduler = std::make_shared<IoScheduler>(2, "HookTest");
  io_scheduler->start();

  std::atomic<bool> sleep_done{false};
  std::atomic<bool> early_ran{false};
  std::atomic<long long> duration_ms{0};

  // 在 sleep 期间期望能执行的短任务（100ms 后执行）
  io_scheduler->add_timer(100, [&]() { early_ran = true; });

  io_scheduler->schedule([&]() {
    auto start = std::chrono::steady_clock::now();
    sleep(1); // 期望被 hook 为异步定时器并挂起当前 fiber
    auto end = std::chrono::steady_clock::now();

    duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
            .count();
    sleep_done = true;
  });

  // 等待任务完成（避免 CI/系统抖动导致固定 sleep 时间不足）
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (sleep_done.load(std::memory_order_acquire) &&
        early_ran.load(std::memory_order_acquire)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // 断言：sleep 的任务确实完成，且早期定时器在 sleep 完成之前或期间被触发
  EXPECT_TRUE(sleep_done.load());
  EXPECT_TRUE(early_ran.load());

  EXPECT_GE(duration_ms.load(), 900); // 不小于约 900ms
  EXPECT_LE(duration_ms.load(), 3000); // 不超出太多（防止无限阻塞情况误判）

  io_scheduler->stop();
}

int main(int argc, char **argv) {
  // 初始化日志系统
  zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}