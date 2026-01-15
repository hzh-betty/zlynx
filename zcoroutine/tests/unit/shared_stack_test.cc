/**
 * @file shared_stack_test.cc
 * @brief 共享栈模式详细单元测试
 */

#include "runtime/fiber.h"
#include "scheduling/scheduler.h"
#include "util/thread_context.h"
#include "util/zcoroutine_logger.h"
#include <atomic>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <vector>

using namespace zcoroutine;

class SharedStackTest : public ::testing::Test {
protected:
  void SetUp() override {
    // 每个测试前重置配置
    ThreadContext::reset_shared_stack_config();
  }

  void TearDown() override {
    // 每个测试后重置配置
    ThreadContext::reset_shared_stack_config();
  }
};


// 测试1：SharedStackBuffer 创建
TEST_F(SharedStackTest, SharedStackBufferCreation) {
  const size_t stack_size = 64 * 1024;
  SharedStackBuffer buffer(stack_size);

  EXPECT_NE(buffer.buffer(), nullptr);
  EXPECT_EQ(buffer.size(), stack_size);
  EXPECT_NE(buffer.stack_top(), nullptr);
  EXPECT_EQ(buffer.occupy_fiber(), nullptr);

  // 验证栈顶 = buffer + size
  EXPECT_EQ(buffer.stack_top(), buffer.buffer() + stack_size);
}

// 测试2：SharedStackBuffer 占用状态
TEST_F(SharedStackTest, SharedStackBufferOccupy) {
  SharedStackBuffer buffer(64 * 1024);

  EXPECT_EQ(buffer.occupy_fiber(), nullptr);

  // 模拟设置占用
  Fiber *fake_fiber = reinterpret_cast<Fiber *>(0x12345678);
  buffer.set_occupy_fiber(fake_fiber);
  EXPECT_EQ(buffer.occupy_fiber(), fake_fiber);

  buffer.set_occupy_fiber(nullptr);
  EXPECT_EQ(buffer.occupy_fiber(), nullptr);
}

// 测试3：SharedStack 创建
TEST_F(SharedStackTest, SharedStackCreation) {
  SharedStack stack(4, 128 * 1024);

  EXPECT_EQ(stack.count(), 4);
  EXPECT_EQ(stack.stack_size(), 128 * 1024);
}

// 测试4：SharedStack 默认值创建
TEST_F(SharedStackTest, SharedStackDefaultCreation) {
  SharedStack stack;

  EXPECT_EQ(stack.count(), SharedStack::kDefaultStackCount);
  EXPECT_EQ(stack.stack_size(), SharedStack::kDefaultStackSize);
}

// 测试5：SharedStack 轮询分配
TEST_F(SharedStackTest, SharedStackAllocateRoundRobin) {
  const int count = 4;
  SharedStack stack(count, 64 * 1024);

  std::vector<SharedStackBuffer *> buffers;

  // 分配多次，验证轮询
  for (int i = 0; i < count * 3; ++i) {
    SharedStackBuffer *buf = stack.allocate();
    EXPECT_NE(buf, nullptr);
    buffers.push_back(buf);
  }

  // 验证轮询模式
  for (int i = count; i < count * 3; ++i) {
    EXPECT_EQ(buffers[i], buffers[i % count]);
  }
}


// 测试6：ThreadContext 默认模式
TEST_F(SharedStackTest, ThreadContextDefault) {
  EXPECT_EQ(ThreadContext::get_stack_mode(), StackMode::kIndependent);
}

// 测试7：ThreadContext 设置共享栈模式
TEST_F(SharedStackTest, ThreadContextSetSharedMode) {
  ThreadContext::set_stack_mode(StackMode::kShared);
  EXPECT_EQ(ThreadContext::get_stack_mode(), StackMode::kShared);

  ThreadContext::set_stack_mode(StackMode::kIndependent);
  EXPECT_EQ(ThreadContext::get_stack_mode(), StackMode::kIndependent);
}

// 测试8：ThreadContext 自动创建共享栈
TEST_F(SharedStackTest, ThreadContextAutoCreateSharedStack) {
  ThreadContext::set_stack_mode(StackMode::kShared);

  SharedStack *stack = ThreadContext::get_shared_stack();
  EXPECT_NE(stack, nullptr);
  EXPECT_EQ(stack->count(), SharedStack::kDefaultStackCount);
}

// 测试9：ThreadContext 自定义共享栈
TEST_F(SharedStackTest, ThreadContextCustomSharedStack) {
  auto custom_stack = std::make_unique<SharedStack>(8, 256 * 1024);
  ThreadContext::set_shared_stack(std::move(custom_stack));
  ThreadContext::set_stack_mode(StackMode::kShared);

  SharedStack *stack = ThreadContext::get_shared_stack();
  EXPECT_EQ(stack->count(), 8);
  EXPECT_EQ(stack->stack_size(), 256 * 1024);
}

// 测试510：ThreadContext 重置
TEST_F(SharedStackTest, ThreadContextReset) {
  ThreadContext::set_stack_mode(StackMode::kShared);
  ThreadContext::reset_shared_stack_config();

  EXPECT_EQ(ThreadContext::get_stack_mode(), StackMode::kIndependent);
}


// 测试11：共享栈模式创建协程
TEST_F(SharedStackTest, CreateFiberWithSharedStack) {
  ThreadContext::set_stack_mode(StackMode::kShared);

  bool executed = false;
  auto fiber = std::make_shared<Fiber>([&executed]() { executed = true; });

  EXPECT_TRUE(fiber->is_shared_stack());
  EXPECT_EQ(fiber->stack_mode(), StackMode::kShared);
  EXPECT_EQ(fiber->state(), Fiber::State::kReady);
  EXPECT_FALSE(executed);

  fiber->resume();
  EXPECT_TRUE(executed);
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

// 测试12：独立栈模式创建协程
TEST_F(SharedStackTest, CreateFiberWithIndependentStack) {
  ThreadContext::set_stack_mode(StackMode::kIndependent);

  bool executed = false;
  auto fiber = std::make_shared<Fiber>([&executed]() { executed = true; });

  EXPECT_FALSE(fiber->is_shared_stack());
  EXPECT_EQ(fiber->stack_mode(), StackMode::kIndependent);

  fiber->resume();
  EXPECT_TRUE(executed);
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

// 测试13：显式指定使用共享栈
TEST_F(SharedStackTest, CreateFiberExplicitSharedStack) {
  // 设置共享栈模式以确保有共享栈可用
  ThreadContext::set_stack_mode(StackMode::kShared);

  bool executed = false;
  auto fiber =
      std::make_shared<Fiber>([&executed]() { executed = true; },
                              StackAllocator::kDefaultStackSize, "", true);

  EXPECT_TRUE(fiber->is_shared_stack());

  fiber->resume();
  EXPECT_TRUE(executed);
}

// 测试14：使用指定SharedStack创建协程
TEST_F(SharedStackTest, CreateFiberWithExplicitSharedStack) {
  auto shared_stack = std::make_shared<SharedStack>(2, 64 * 1024);

  bool executed = false;
  auto fiber = std::make_shared<Fiber>([&executed]() { executed = true; },
                                       shared_stack.get());

  EXPECT_TRUE(fiber->is_shared_stack());

  fiber->resume();
  EXPECT_TRUE(executed);
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}


// 测试15：共享栈协程单次yield
TEST_F(SharedStackTest, SharedStackFiberYield) {
  ThreadContext::set_stack_mode(StackMode::kShared);

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

// 测试16：共享栈协程多次yield
TEST_F(SharedStackTest, SharedStackFiberMultipleYields) {
  ThreadContext::set_stack_mode(StackMode::kShared);

  std::vector<int> steps;

  auto fiber = std::make_shared<Fiber>([&steps]() {
    for (int i = 0; i < 5; ++i) {
      steps.push_back(i);
      Fiber::yield();
    }
  });

  for (int i = 0; i < 5; ++i) {
    fiber->resume();
    EXPECT_EQ(steps.size(), static_cast<size_t>(i + 1));
    EXPECT_EQ(steps[i], i);
    if (i < 4) {
      EXPECT_EQ(fiber->state(), Fiber::State::kSuspended);
    }
  }

  fiber->resume();
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}


// 测试17：多协程共享同一栈缓冲区
TEST_F(SharedStackTest, MultipleFibersShareSameBuffer) {
  // 创建只有1个缓冲区的共享栈，强制所有协程共享
  auto shared_stack = std::make_shared<SharedStack>(1, 128 * 1024);

  std::vector<int> execution_order;

  auto fiber1 = std::make_shared<Fiber>(
      [&execution_order]() {
        execution_order.push_back(1);
        Fiber::yield();
        execution_order.push_back(3);
      },
      shared_stack.get(), "fiber1");

  auto fiber2 = std::make_shared<Fiber>(
      [&execution_order]() {
        execution_order.push_back(2);
        Fiber::yield();
        execution_order.push_back(4);
      },
      shared_stack.get(), "fiber2");

  // 交替执行
  fiber1->resume(); // 1
  EXPECT_EQ(execution_order.back(), 1);

  fiber2->resume(); // 2
  EXPECT_EQ(execution_order.back(), 2);

  fiber1->resume(); // 3
  EXPECT_EQ(execution_order.back(), 3);

  fiber2->resume(); // 4
  EXPECT_EQ(execution_order.back(), 4);

  EXPECT_EQ(execution_order.size(), 4);
  EXPECT_EQ(fiber1->state(), Fiber::State::kTerminated);
  EXPECT_EQ(fiber2->state(), Fiber::State::kTerminated);
}

// 测试18：多协程交替执行验证数据完整性
TEST_F(SharedStackTest, MultipleFibersDataIntegrity) {
  auto shared_stack = std::make_shared<SharedStack>(1, 128 * 1024);

  int value1 = 0;
  int value2 = 0;

  auto fiber1 = std::make_shared<Fiber>(
      [&value1]() {
        int local_val = 100;
        value1 = local_val;
        Fiber::yield();
        // 验证局部变量在resume后仍然正确
        EXPECT_EQ(local_val, 100);
        value1 = local_val + 10;
      },
      shared_stack.get());

  auto fiber2 = std::make_shared<Fiber>(
      [&value2]() {
        int local_val = 200;
        value2 = local_val;
        Fiber::yield();
        // 验证局部变量在resume后仍然正确
        EXPECT_EQ(local_val, 200);
        value2 = local_val + 20;
      },
      shared_stack.get());

  fiber1->resume();
  EXPECT_EQ(value1, 100);

  fiber2->resume();
  EXPECT_EQ(value2, 200);

  fiber1->resume();
  EXPECT_EQ(value1, 110);

  fiber2->resume();
  EXPECT_EQ(value2, 220);
}

// 测试19：大量协程共享栈
TEST_F(SharedStackTest, ManyFibersShareStack) {
  // 4个缓冲区，100个协程
  auto shared_stack = std::make_shared<SharedStack>(4, 128 * 1024);
  const int fiber_count = 100;

  std::vector<Fiber::ptr> fibers;
  std::atomic<int> counter{0};

  for (int i = 0; i < fiber_count; ++i) {
    fibers.push_back(std::make_shared<Fiber>([&counter]() { counter++; },
                                             shared_stack.get()));
  }

  for (auto &fiber : fibers) {
    fiber->resume();
  }

  EXPECT_EQ(counter.load(), fiber_count);

  for (auto &fiber : fibers) {
    EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
  }
}

// 测试20：共享栈协程重置复用
TEST_F(SharedStackTest, SharedStackFiberReset) {
  ThreadContext::set_stack_mode(StackMode::kShared);

  int count = 0;

  auto fiber = std::make_shared<Fiber>([&count]() { count++; });

  EXPECT_TRUE(fiber->is_shared_stack());

  fiber->resume();
  EXPECT_EQ(count, 1);
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);

  fiber->reset([&count]() { count += 10; });
  EXPECT_EQ(fiber->state(), Fiber::State::kReady);

  fiber->resume();
  EXPECT_EQ(count, 11);
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}


// 测试21：栈上复杂数据类型保存恢复
TEST_F(SharedStackTest, SharedStackComplexDataPreservation) {
  auto shared_stack = std::make_shared<SharedStack>(1, 128 * 1024);

  struct ComplexData {
    int values[10];
    double pi;
    char text[32];
  };

  ComplexData result1, result2;

  auto fiber1 = std::make_shared<Fiber>(
      [&result1]() {
        ComplexData local;
        for (int i = 0; i < 10; ++i) {
          local.values[i] = i * 10;
        }
        local.pi = 3.14159;
        strcpy(local.text, "fiber1");

        Fiber::yield();

        // 验证数据保持不变
        result1 = local;
      },
      shared_stack.get());

  auto fiber2 = std::make_shared<Fiber>(
      [&result2]() {
        ComplexData local;
        for (int i = 0; i < 10; ++i) {
          local.values[i] = i * 100;
        }
        local.pi = 2.71828;
        strcpy(local.text, "fiber2");

        Fiber::yield();

        // 验证数据保持不变
        result2 = local;
      },
      shared_stack.get());

  fiber1->resume();
  fiber2->resume();
  fiber1->resume();
  fiber2->resume();

  // 验证fiber1数据
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(result1.values[i], i * 10);
  }
  EXPECT_DOUBLE_EQ(result1.pi, 3.14159);
  EXPECT_STREQ(result1.text, "fiber1");

  // 验证fiber2数据
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(result2.values[i], i * 100);
  }
  EXPECT_DOUBLE_EQ(result2.pi, 2.71828);
  EXPECT_STREQ(result2.text, "fiber2");
}

// 测试22：递归函数中的共享栈
TEST_F(SharedStackTest, SharedStackRecursion) {
  auto shared_stack = std::make_shared<SharedStack>(1, 256 * 1024);

  int result1 = 0;
  int result2 = 0;

  std::function<int(int)> fibonacci = [&fibonacci](int n) -> int {
    if (n <= 1)
      return n;
    return fibonacci(n - 1) + fibonacci(n - 2);
  };

  auto fiber1 = std::make_shared<Fiber>(
      [&result1, &fibonacci]() {
        result1 = fibonacci(10);
        Fiber::yield();
        result1 = fibonacci(15);
      },
      shared_stack.get());

  auto fiber2 = std::make_shared<Fiber>(
      [&result2, &fibonacci]() {
        result2 = fibonacci(12);
        Fiber::yield();
        result2 = fibonacci(8);
      },
      shared_stack.get());

  fiber1->resume();
  EXPECT_EQ(result1, 55); // fib(10) = 55

  fiber2->resume();
  EXPECT_EQ(result2, 144); // fib(12) = 144

  fiber1->resume();
  EXPECT_EQ(result1, 610); // fib(15) = 610

  fiber2->resume();
  EXPECT_EQ(result2, 21); // fib(8) = 21
}


// 测试23：共享栈协程内部异常捕获
TEST_F(SharedStackTest, SharedStackExceptionCatch) {
  ThreadContext::set_stack_mode(StackMode::kShared);

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

// 测试24：共享栈协程yield后异常
TEST_F(SharedStackTest, SharedStackExceptionAfterYield) {
  ThreadContext::set_stack_mode(StackMode::kShared);

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


// 测试25：独立栈和共享栈协程混合执行
TEST_F(SharedStackTest, MixedStackModes) {
  auto shared_stack = std::make_shared<SharedStack>(2, 128 * 1024);

  std::vector<int> order;

  // 共享栈协程
  auto shared_fiber1 = std::make_shared<Fiber>(
      [&order]() {
        order.push_back(1);
        Fiber::yield();
        order.push_back(4);
      },
      shared_stack.get());

  // 独立栈协程
  auto independent_fiber = std::make_shared<Fiber>([&order]() {
    order.push_back(2);
    Fiber::yield();
    order.push_back(5);
  });

  // 共享栈协程
  auto shared_fiber2 = std::make_shared<Fiber>(
      [&order]() {
        order.push_back(3);
        Fiber::yield();
        order.push_back(6);
      },
      shared_stack.get());

  EXPECT_TRUE(shared_fiber1->is_shared_stack());
  EXPECT_FALSE(independent_fiber->is_shared_stack());
  EXPECT_TRUE(shared_fiber2->is_shared_stack());

  // 交替执行
  shared_fiber1->resume();
  independent_fiber->resume();
  shared_fiber2->resume();
  shared_fiber1->resume();
  independent_fiber->resume();
  shared_fiber2->resume();

  EXPECT_EQ(order.size(), 6);
  for (int i = 0; i < 6; ++i) {
    EXPECT_EQ(order[i], i + 1);
  }
}


// 测试26：空函数共享栈协程
TEST_F(SharedStackTest, SharedStackEmptyFunction) {
  ThreadContext::set_stack_mode(StackMode::kShared);

  auto fiber = std::make_shared<Fiber>([]() {});

  fiber->resume();
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

// 测试27：立即yield的共享栈协程
TEST_F(SharedStackTest, SharedStackImmediateYield) {
  ThreadContext::set_stack_mode(StackMode::kShared);

  auto fiber = std::make_shared<Fiber>([]() { Fiber::yield(); });

  fiber->resume();
  EXPECT_EQ(fiber->state(), Fiber::State::kSuspended);

  fiber->resume();
  EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

// 测试28：大量yield操作
TEST_F(SharedStackTest, SharedStackManyYields) {
  ThreadContext::set_stack_mode(StackMode::kShared);

  const int yield_count = 500;
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


// 测试29：多线程独立的共享栈配置
TEST_F(SharedStackTest, ThreadLocalStackConfig) {
  const int thread_count = 4;
  std::vector<std::thread> threads;
  std::atomic<int> shared_count{0};
  std::atomic<int> independent_count{0};

  for (int t = 0; t < thread_count; ++t) {
    threads.emplace_back([t, &shared_count, &independent_count]() {
      // 偶数线程使用共享栈，奇数线程使用独立栈
      if (t % 2 == 0) {
        ThreadContext::set_stack_mode(StackMode::kShared);
      } else {
        ThreadContext::set_stack_mode(StackMode::kIndependent);
      }

      auto fiber =
          std::make_shared<Fiber>([t, &shared_count, &independent_count]() {
            if (t % 2 == 0) {
              shared_count++;
            } else {
              independent_count++;
            }
          });

      // 验证栈模式正确
      EXPECT_EQ(fiber->is_shared_stack(), (t % 2 == 0));

      fiber->resume();

      ThreadContext::reset_shared_stack_config();
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(shared_count.load(), thread_count / 2);
  EXPECT_EQ(independent_count.load(), thread_count / 2);
}

// 测试30：每个线程独立的共享栈
TEST_F(SharedStackTest, PerThreadSharedStack) {
  const int thread_count = 4;
  const int fibers_per_thread = 10;
  std::vector<std::thread> threads;
  std::atomic<int> total_count{0};

  for (int t = 0; t < thread_count; ++t) {
    threads.emplace_back([&total_count, fibers_per_thread]() {
      auto shared_stack = std::make_shared<SharedStack>(2, 64 * 1024);

      std::vector<Fiber::ptr> fibers;
      int local_count = 0;

      for (int i = 0; i < fibers_per_thread; ++i) {
        fibers.push_back(std::make_shared<Fiber>(
            [&local_count]() {
              local_count++;
              Fiber::yield();
              local_count++;
            },
            shared_stack.get()));
      }

      // 第一轮执行
      for (auto &fiber : fibers) {
        fiber->resume();
      }
      EXPECT_EQ(local_count, fibers_per_thread);

      // 第二轮执行
      for (auto &fiber : fibers) {
        fiber->resume();
      }
      EXPECT_EQ(local_count, fibers_per_thread * 2);

      total_count += local_count;
    });
  }

  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(total_count.load(), thread_count * fibers_per_thread * 2);
}

// 测试31：共享栈协程创建销毁
TEST_F(SharedStackTest, SharedStackFiberCreationDestruction) {
  auto shared_stack = std::make_shared<SharedStack>(4, 64 * 1024);
  const int count = 1000;

  for (int i = 0; i < count; ++i) {
    auto fiber = std::make_shared<Fiber>([]() {}, shared_stack.get());
    fiber->resume();
    EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
  }
}

// 测试32：共享栈协程频繁切换
TEST_F(SharedStackTest, SharedStackFrequentSwitch) {
  auto shared_stack = std::make_shared<SharedStack>(2, 64 * 1024);

  int count1 = 0, count2 = 0;

  auto fiber1 = std::make_shared<Fiber>(
      [&count1]() {
        for (int i = 0; i < 100; ++i) {
          count1++;
          Fiber::yield();
        }
      },
      shared_stack.get());

  auto fiber2 = std::make_shared<Fiber>(
      [&count2]() {
        for (int i = 0; i < 100; ++i) {
          count2++;
          Fiber::yield();
        }
      },
      shared_stack.get());

  // 交替执行
  while (fiber1->state() != Fiber::State::kTerminated ||
         fiber2->state() != Fiber::State::kTerminated) {
    if (fiber1->state() != Fiber::State::kTerminated) {
      fiber1->resume();
    }
    if (fiber2->state() != Fiber::State::kTerminated) {
      fiber2->resume();
    }
  }

  EXPECT_EQ(count1, 100);
  EXPECT_EQ(count2, 100);
}

// 测试33：Scheduler共享栈模式创建
TEST_F(SharedStackTest, SchedulerSharedStackMode) {
  // 创建使用共享栈模式的调度器
  auto scheduler = std::make_shared<Scheduler>(1, "SharedStackScheduler", true);

  EXPECT_TRUE(scheduler->is_shared_stack());
  // SharedStack不再作为Scheduler成员，而是在worker线程中通过ThreadContext创建
}

// 测试34：Scheduler独立栈模式创建
TEST_F(SharedStackTest, SchedulerIndependentStackMode) {
  // 创建使用独立栈模式的调度器
  auto scheduler =
      std::make_shared<Scheduler>(1, "IndependentStackScheduler", false);

  EXPECT_FALSE(scheduler->is_shared_stack());
  // SharedStack不再作为Scheduler成员
}

// 测试35：Scheduler共享栈模式执行任务
TEST_F(SharedStackTest, SchedulerSharedStackExecution) {
  auto scheduler = std::make_shared<Scheduler>(2, "SharedStackScheduler", true);
  std::atomic<int> counter{0};
  const int task_count = 20;

  scheduler->start();

  for (int i = 0; i < task_count; ++i) {
    scheduler->schedule([&counter]() { counter++; });
  }

  // 等待任务完成
  while (scheduler->pending_task_count() > 0 || counter.load() < task_count) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  scheduler->stop();

  EXPECT_EQ(counter.load(), task_count);
}

TEST_F(SharedStackTest, SharedStackInvalidParams) {
  // 测试构造函数非法参数，应使用默认值
  SharedStack stack(-1, 0);
  EXPECT_EQ(stack.count(), SharedStack::kDefaultStackCount);
  EXPECT_EQ(stack.stack_size(), SharedStack::kDefaultStackSize);
}

TEST_F(SharedStackTest, SharedContextSaveRestoreEdgeCases) {
  SharedContext ctx;
  size_t stack_size = 4096;
  SharedStackBuffer buffer(stack_size);

  // 1. 未初始化/空指针测试
  ctx.save_stack_buffer(nullptr); // Should return early
  ctx.restore_stack_buffer();     // Should return early

  ctx.init_shared(&buffer);

  char *stack_top = buffer.stack_top();

  // 2. save_stack_buffer 非法 stack_sp (>= top)
  ctx.save_stack_buffer(stack_top); // Should log warn and return
  EXPECT_EQ(ctx.save_size(), 0);

  ctx.save_stack_buffer(stack_top + 10);
  EXPECT_EQ(ctx.save_size(), 0);

  // 4. 正常保存
  char *sp = stack_top - 100;
  // 模拟栈内容
  for (int i = 0; i < 100; ++i)
    *(sp + i) = (char)i;

  ctx.save_stack_buffer(sp);
  EXPECT_EQ(ctx.save_size(), 100);
  EXPECT_NE(ctx.save_buffer(), nullptr);
  EXPECT_EQ(ctx.saved_stack_sp(), sp);

  // 5. 正常恢复
  // 先修改栈内容
  memset(sp, 0, 100);
  ctx.restore_stack_buffer();
  // 验证恢复
  for (int i = 0; i < 100; ++i)
    EXPECT_EQ(*(sp + i), (char)i);

  SharedStackBuffer buffer2(stack_size);
  ctx.init_shared(&buffer2); // 切换到 buffer2

  ctx.restore_stack_buffer();
}

TEST_F(SharedStackTest, SharedContextReset) {
  SharedContext ctx;
  size_t stack_size = 4096;
  SharedStackBuffer buffer(stack_size);
  ctx.init_shared(&buffer);

  char *sp = buffer.stack_top() - 100;
  ctx.save_stack_buffer(sp);
  EXPECT_NE(ctx.save_buffer(), nullptr);
  EXPECT_NE(ctx.saved_stack_sp(), nullptr);

  ctx.reset();
  EXPECT_EQ(ctx.save_size(), 0);
  EXPECT_EQ(ctx.saved_stack_sp(), nullptr);
}

TEST_F(SharedStackTest, SwitchStackAllocation) {
  // 覆盖 SwitchStack 构造函数
  SwitchStack ss(4096);
  EXPECT_NE(ss.buffer(), nullptr);
  EXPECT_EQ(ss.size(), 4096);
}

int main(int argc, char **argv) {
  // 初始化日志系统
  zcoroutine::init_logger(zlog::LogLevel::value::DEBUG);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
