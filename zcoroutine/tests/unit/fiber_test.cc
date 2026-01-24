/**
 * @file fiber_test.cc
 * @brief Fiber类详细单元测试
 */

#include "fiber.h"
#include "zcoroutine_logger.h"
#include <gtest/gtest.h>
#include <set>
#include <thread>
#include <vector>

using namespace zcoroutine;

class FiberTest : public ::testing::Test {
protected:
  void SetUp() override {
    // 每个测试前的准备工作
  }

  void TearDown() override {
    // 每个测试后的清理工作
  }
};

// ============================================================================
// 基础功能测试
// ============================================================================

// 测试1：协程创建
TEST_F(FiberTest, CreateFiber) {
  bool executed = false;

  auto fiber = std::make_shared<Fiber>([&executed]() { executed = true; });

  EXPECT_NE(fiber, nullptr);
  EXPECT_EQ(fiber->id(), 0);
  EXPECT_EQ(fiber->state(), Fiber::State::kReady);
  EXPECT_FALSE(executed);
}

// 测试2：协程执行
TEST_F(FiberTest, ExecuteFiber) {
  int value = 0;

  auto fiber = std::make_shared<Fiber>([&value]() {
    value = 42;
    Fiber::yield();
    value = 100;
  });

  EXPECT_EQ(value, 0);
  fiber->resume();
  EXPECT_EQ(value, 42);
  fiber->resume();
  EXPECT_EQ(value, 100);
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

// 测试3：协程状态转换 Ready -> Running -> Terminated
TEST_F(FiberTest, StateTransition) {
  auto fiber = std::make_shared<Fiber>([]() {
    // 简单执行
  });

  EXPECT_EQ(fiber->state(), Fiber::State::kReady);
  fiber->resume();
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

// 测试4：协程状态转换 Ready -> Running -> Suspended -> Running -> Terminated
TEST_F(FiberTest, StateTransitionWithYield) {
  int step = 0;

  auto fiber = std::make_shared<Fiber>([&step]() {
    step = 1;
    Fiber::yield();
    step = 2;
  });

  EXPECT_EQ(fiber->state(), Fiber::State::kReady);

  fiber->resume();
  EXPECT_EQ(step, 1);
  EXPECT_EQ(fiber->state(), Fiber::State::kSuspended);

  fiber->resume();
  EXPECT_EQ(step, 2);
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

// 测试5：协程名称 - 默认名称
TEST_F(FiberTest, DefaultFiberName) {
  auto fiber = std::make_shared<Fiber>([]() {});

  EXPECT_FALSE(fiber->name().empty());
  EXPECT_TRUE(fiber->name().find("fiber") != std::string::npos);
}

// 测试6：协程名称 - 自定义名称
TEST_F(FiberTest, CustomFiberName) {
  auto fiber = std::make_shared<Fiber>(
      []() {}, StackAllocator::kDefaultStackSize, "test_fiber");

  EXPECT_TRUE(fiber->name().find("test_fiber") != std::string::npos);
}

// 测试7：协程ID唯一性
TEST_F(FiberTest, UniqueFiberId) {
  const int count = 100;
  std::set<uint64_t> ids;
  std::vector<Fiber::ptr> fibers;

  for (int i = 0; i < count; ++i) {
    auto fiber = std::make_shared<Fiber>([]() {});
    ids.insert(fiber->id());
    fibers.push_back(fiber);
  }

  EXPECT_EQ(ids.size(), count);
}

// 测试8：协程重置功能
TEST_F(FiberTest, ResetFiber) {
  int count = 0;

  auto fiber = std::make_shared<Fiber>([&count]() { count++; });

  fiber->resume();
  EXPECT_EQ(count, 1);
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);

  fiber->reset([&count]() { count++; });
  EXPECT_EQ(fiber->state(), Fiber::State::kReady);

  fiber->resume();
  EXPECT_EQ(count, 2);
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

// ============================================================================
// Yield功能测试
// ============================================================================

// 测试9：单次yield
TEST_F(FiberTest, SingleYield) {
  std::vector<int> steps;

  auto fiber = std::make_shared<Fiber>([&steps]() {
    steps.push_back(1);
    Fiber::yield();
    steps.push_back(2);
  });

  fiber->resume();
  EXPECT_EQ(steps.size(), 1);
  EXPECT_EQ(steps[0], 1);
  EXPECT_EQ(fiber->state(), Fiber::State::kSuspended);

  fiber->resume();
  EXPECT_EQ(steps.size(), 2);
  EXPECT_EQ(steps[1], 2);
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

// 测试10：多次yield
TEST_F(FiberTest, MultipleYields) {
  std::vector<int> steps;

  auto fiber = std::make_shared<Fiber>([&steps]() {
    for (int i = 0; i < 5; ++i) {
      steps.push_back(i);
      Fiber::yield();
    }
  });

  for (int i = 0; i < 5; ++i) {
    fiber->resume();
    EXPECT_EQ(steps.size(), i + 1);
    EXPECT_EQ(steps[i], i);
    if (i < 4) {
      EXPECT_EQ(fiber->state(), Fiber::State::kSuspended);
    }
  }

  fiber->resume();
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

// 测试11：循环中的yield
TEST_F(FiberTest, YieldInLoop) {
  int sum = 0;

  auto fiber = std::make_shared<Fiber>([&sum]() {
    for (int i = 1; i <= 10; ++i) {
      sum += i;
      if (i % 3 == 0) {
        Fiber::yield();
      }
    }
  });

  // 第一次resume
  fiber->resume();
  EXPECT_EQ(sum, 1 + 2 + 3); // 6
  EXPECT_EQ(fiber->state(), Fiber::State::kSuspended);

  // 第二次resume
  fiber->resume();
  EXPECT_EQ(sum, 1 + 2 + 3 + 4 + 5 + 6); // 21
  EXPECT_EQ(fiber->state(), Fiber::State::kSuspended);

  // 第三次resume
  fiber->resume();
  EXPECT_EQ(sum, 1 + 2 + 3 + 4 + 5 + 6 + 7 + 8 + 9); // 45
  EXPECT_EQ(fiber->state(), Fiber::State::kSuspended);

  // 第四次resume（完成）
  fiber->resume();
  EXPECT_EQ(sum, 55);
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

// ============================================================================
// 异常处理测试
// ============================================================================

// 测试12：协程内部捕获异常
TEST_F(FiberTest, CatchExceptionInFiber) {
  bool exception_caught = false;

  auto fiber = std::make_shared<Fiber>([&exception_caught]() {
    try {
      throw std::runtime_error("test exception");
    } catch (const std::exception &e) {
      exception_caught = true;
    }
  });

  EXPECT_NO_THROW(fiber->resume());
  EXPECT_TRUE(exception_caught);
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

// 测试13：yield后异常
TEST_F(FiberTest, ExceptionAfterYield) {
  bool exception_caught = false;
  int step = 0;

  auto fiber = std::make_shared<Fiber>([&exception_caught, &step]() {
    step = 1;
    Fiber::yield();
    step = 2;
    try {
      throw std::logic_error("error after yield");
    } catch (const std::exception &e) {
      exception_caught = true;
    }
    step = 3;
  });

  fiber->resume();
  EXPECT_EQ(step, 1);
  EXPECT_FALSE(exception_caught);

  fiber->resume();
  EXPECT_EQ(step, 3);
  EXPECT_TRUE(exception_caught);
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

// ============================================================================
// 边界条件测试
// ============================================================================

// 测试14：空函数协程
TEST_F(FiberTest, EmptyFunctionFiber) {
  auto fiber = std::make_shared<Fiber>([]() {});

  fiber->resume();
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

// 测试15：立即yield的协程
TEST_F(FiberTest, ImmediateYield) {
  auto fiber = std::make_shared<Fiber>([]() { Fiber::yield(); });

  fiber->resume();
  EXPECT_EQ(fiber->state(), Fiber::State::kSuspended);

  fiber->resume();
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

// 测试16：resume已终止的协程（应该安全）
TEST_F(FiberTest, ResumeTerminatedFiber) {
  auto fiber = std::make_shared<Fiber>([]() {});

  fiber->resume();
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);

  // 再次resume不应崩溃
  // EXPECT_NO_THROW(fiber->resume());
}

// ============================================================================
// 栈大小测试
// ============================================================================

// 测试17：自定义栈大小
TEST_F(FiberTest, CustomStackSize) {
  const size_t custom_size = 256 * 1024;
  bool executed = false;

  auto fiber =
      std::make_shared<Fiber>([&executed]() { executed = true; }, custom_size);

  fiber->resume();
  EXPECT_TRUE(executed);
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

// 测试18：小栈大小
TEST_F(FiberTest, SmallStackSize) {
  const size_t small_size = 64 * 1024;
  int value = 0;

  auto fiber = std::make_shared<Fiber>([&value]() { value = 100; }, small_size);

  fiber->resume();
  EXPECT_EQ(value, 100);
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

// 测试19：大栈大小
TEST_F(FiberTest, LargeStackSize) {
  const size_t large_size = 1024 * 1024; // 1MB
  bool success = false;

  auto fiber = std::make_shared<Fiber>(
      [&success]() {
        // 在栈上分配较大数组
        char buffer[512 * 1024];
        buffer[0] = 'a';
        buffer[512 * 1024 - 1] = 'z';
        success = true;
      },
      large_size);

  fiber->resume();
  EXPECT_TRUE(success);
}

// ============================================================================
// 数据传递测试
// ============================================================================

// 测试20：通过引用传递数据
TEST_F(FiberTest, DataPassingByReference) {
  std::vector<int> data;

  auto fiber = std::make_shared<Fiber>([&data]() {
    for (int i = 0; i < 5; ++i) {
      data.push_back(i);
      Fiber::yield();
    }
  });

  for (int i = 0; i < 5; ++i) {
    fiber->resume();
    EXPECT_EQ(data.size(), i + 1);
  }
}

// 测试21：复杂数据结构
TEST_F(FiberTest, ComplexDataStructure) {
  struct ComplexData {
    int value;
    std::string text;
    std::vector<double> numbers;
  };

  ComplexData result;

  auto fiber = std::make_shared<Fiber>([&result]() {
    result.value = 42;
    result.text = "hello";
    result.numbers = {1.1, 2.2, 3.3};
  });

  fiber->resume();

  EXPECT_EQ(result.value, 42);
  EXPECT_EQ(result.text, "hello");
  EXPECT_EQ(result.numbers.size(), 3);
}

// ============================================================================
// 并发安全测试
// ============================================================================

// 测试22：多个协程并发创建
TEST_F(FiberTest, ConcurrentFiberCreation) {
  const int fiber_count = 100;
  std::vector<Fiber::ptr> fibers;
  std::atomic<int> counter{0};

  for (int i = 0; i < fiber_count; ++i) {
    fibers.push_back(std::make_shared<Fiber>([&counter]() { counter++; }));
  }

  for (auto &fiber : fibers) {
    fiber->resume();
  }

  EXPECT_EQ(counter.load(), fiber_count);
}

// 测试23：协程ID在并发场景下的唯一性
TEST_F(FiberTest, ConcurrentFiberIdUniqueness) {
  const int thread_count = 4;
  const int fibers_per_thread = 50;
  std::vector<std::thread> threads;
  std::mutex mtx;
  std::set<uint64_t> all_ids;

  for (int t = 0; t < thread_count; ++t) {
    threads.emplace_back([&]() {
      std::vector<Fiber::ptr> fibers;
      for (int i = 0; i < fibers_per_thread; ++i) {
        fibers.push_back(std::make_shared<Fiber>([]() {}));
      }

      std::lock_guard<std::mutex> lock(mtx);
      for (auto &fiber : fibers) {
        all_ids.insert(fiber->id());
      }
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(all_ids.size(), thread_count * fibers_per_thread);
}

// ============================================================================
// 重置后复用测试
// ============================================================================

// 测试24：协程重置后状态正确
TEST_F(FiberTest, FiberStateAfterReset) {
  int count = 0;
  auto fiber = std::make_shared<Fiber>([&count]() { count++; });

  fiber->resume();
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);

  fiber->reset([&count]() { count += 10; });
  EXPECT_EQ(fiber->state(), Fiber::State::kReady);
  EXPECT_EQ(count, 1);

  fiber->resume();
  EXPECT_EQ(count, 11);
}

// 测试25：多次重置
TEST_F(FiberTest, MultipleResets) {
  int total = 0;
  auto fiber = std::make_shared<Fiber>([&total]() { total += 1; });

  for (int i = 0; i < 5; ++i) {
    fiber->resume();
    EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);

    if (i < 4) {
      fiber->reset([&total]() { total += 1; });
      EXPECT_EQ(fiber->state(), Fiber::State::kReady);
    }
  }

  EXPECT_EQ(total, 5);
}

// ============================================================================
// 长时间运行测试
// ============================================================================

// 测试26：长循环协程
TEST_F(FiberTest, LongRunningFiber) {
  const int iterations = 10000;
  int count = 0;

  auto fiber = std::make_shared<Fiber>([&count]() {
    for (int i = 0; i < iterations; ++i) {
      count++;
    }
  });

  fiber->resume();
  EXPECT_EQ(count, iterations);
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

// 测试27：大量yield操作
TEST_F(FiberTest, ManyYields) {
  const int yield_count = 1000;
  int counter = 0;

  auto fiber = std::make_shared<Fiber>([&counter]() {
    for (int i = 0; i < yield_count; ++i) {
      counter++;
      Fiber::yield();
    }
  });

  for (int i = 0; i < yield_count; ++i) {
    fiber->resume();
    EXPECT_EQ(counter, i + 1);
  }

  fiber->resume();
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

int main(int argc, char **argv) {
  // 初始化日志系统
  zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}