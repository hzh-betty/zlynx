#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <vector>
#include "scheduling/scheduler.h"
#include "scheduling/fiber_pool.h"
#include "runtime/fiber.h"

using namespace zcoroutine;

class SchedulerFiberIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        scheduler_ = std::make_shared<Scheduler>(4, "TestScheduler");
        pool_ = std::make_shared<FiberPool>(10, 100);
        scheduler_->set_fiber_pool(pool_);
    }

    void TearDown() override {
        if (scheduler_ && scheduler_->is_running()) {
            scheduler_->stop();
        }
        scheduler_.reset();
        pool_.reset();
    }

    Scheduler::ptr scheduler_;
    FiberPool::ptr pool_;
};

// ==================== 调度器与协程集成测试 ====================

// 测试1：调度器启动和停止
TEST_F(SchedulerFiberIntegrationTest, StartAndStop) {
    EXPECT_FALSE(scheduler_->is_running());
    
    scheduler_->start();
    EXPECT_TRUE(scheduler_->is_running());
    
    scheduler_->stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(scheduler_->is_running());
}

// 测试2：调度器执行单个协程
TEST_F(SchedulerFiberIntegrationTest, ScheduleSingleFiber) {
    std::atomic<bool> executed{false};
    
    scheduler_->start();
    
    auto fiber = std::make_shared<Fiber>([&executed]() {
        executed.store(true);
    });
    
    scheduler_->schedule(fiber);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(executed.load());
    
    scheduler_->stop();
}

// 测试3：调度器执行多个协程
TEST_F(SchedulerFiberIntegrationTest, ScheduleMultipleFibers) {
    const int fiber_count = 100;
    std::atomic<int> completed{0};
    
    scheduler_->start();
    
    for (int i = 0; i < fiber_count; ++i) {
        auto fiber = std::make_shared<Fiber>([&completed]() {
            completed.fetch_add(1);
        });
        scheduler_->schedule(fiber);
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(completed.load(), fiber_count);
    
    scheduler_->stop();
}

// 测试4：调度器执行回调函数
TEST_F(SchedulerFiberIntegrationTest, ScheduleCallbackFunction) {
    std::atomic<int> sum{0};
    
    scheduler_->start();
    
    for (int i = 1; i <= 10; ++i) {
        scheduler_->schedule([&sum, i]() {
            sum.fetch_add(i);
        });
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(sum.load(), 55); // 1+2+...+10 = 55
    
    scheduler_->stop();
}

// 测试5：协程池与调度器协同工作
TEST_F(SchedulerFiberIntegrationTest, FiberPoolIntegration) {
    std::atomic<int> executed{0};
    
    scheduler_->start();
    
    // 使用协程池创建并调度协程
    for (int i = 0; i < 50; ++i) {
        auto fiber = pool_->get([&executed]() {
            executed.fetch_add(1);
        });
        scheduler_->schedule(fiber);
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(executed.load(), 50);
    
    // 检查协程池统计
    auto stats = pool_->get_statistics();
    EXPECT_GT(stats.total_created, 0);
    
    scheduler_->stop();
}

// 测试6：带yield的协程调度
TEST_F(SchedulerFiberIntegrationTest, FiberWithYield) {
    std::atomic<int> step{0};
    
    scheduler_->start();
    
    auto fiber = std::make_shared<Fiber>([&step]() {
        step.store(1);
        Fiber::yield();
        step.store(2);
        Fiber::yield();
        step.store(3);
    });
    
    scheduler_->schedule(fiber);
    
    // 等待协程完成所有步骤
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(step.load(), 3);
    
    scheduler_->stop();
}

// 测试7：高负载下的调度器
TEST_F(SchedulerFiberIntegrationTest, HighLoadScheduling) {
    const int task_count = 1000;
    std::atomic<int> completed{0};
    
    scheduler_->start();
    
    for (int i = 0; i < task_count; ++i) {
        scheduler_->schedule([&completed, i]() {
            // 模拟一些工作
            int sum = 0;
            for (int j = 0; j < 100; ++j) {
                sum += j;
            }
            completed.fetch_add(1);
        });
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(completed.load(), task_count);
    
    scheduler_->stop();
}

// 测试8：混合协程和回调任务
TEST_F(SchedulerFiberIntegrationTest, MixedFiberAndCallback) {
    std::atomic<int> fiber_count{0};
    std::atomic<int> callback_count{0};
    
    scheduler_->start();
    
    for (int i = 0; i < 50; ++i) {
        if (i % 2 == 0) {
            auto fiber = std::make_shared<Fiber>([&fiber_count]() {
                fiber_count.fetch_add(1);
            });
            scheduler_->schedule(fiber);
        } else {
            scheduler_->schedule([&callback_count]() {
                callback_count.fetch_add(1);
            });
        }
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(fiber_count.load(), 25);
    EXPECT_EQ(callback_count.load(), 25);
    
    scheduler_->stop();
}

// 测试9：协程异常处理
TEST_F(SchedulerFiberIntegrationTest, FiberExceptionHandling) {
    std::atomic<int> completed{0};
    std::atomic<int> failed{0};
    
    scheduler_->start();
    
    for (int i = 0; i < 10; ++i) {
        auto fiber = std::make_shared<Fiber>([&completed, &failed, i]() {
            if (i % 3 == 0) {
                failed.fetch_add(1);
                throw std::runtime_error("Test exception");
            }
            completed.fetch_add(1);
        });
        
        scheduler_->schedule(fiber);
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // 即使有异常，其他协程也应该正常完成
    EXPECT_GT(completed.load(), 0);
    
    scheduler_->stop();
}

// 测试10：调度器线程池大小影响
TEST_F(SchedulerFiberIntegrationTest, ThreadPoolSize) {
    std::atomic<int> completed{0};
    
    // 创建不同线程数的调度器
    auto single_thread = std::make_shared<Scheduler>(1, "Single");
    auto multi_thread = std::make_shared<Scheduler>(4, "Multi");
    
    single_thread->start();
    multi_thread->start();
    
    const int task_count = 100;
    
    // 单线程调度器
    auto start1 = std::chrono::steady_clock::now();
    for (int i = 0; i < task_count; ++i) {
        single_thread->schedule([&completed]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            completed.fetch_add(1);
        });
    }
    while (completed.load() < task_count) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto end1 = std::chrono::steady_clock::now();
    auto duration1 = std::chrono::duration_cast<std::chrono::milliseconds>(end1 - start1).count();
    
    completed.store(0);
    
    // 多线程调度器
    auto start2 = std::chrono::steady_clock::now();
    for (int i = 0; i < task_count; ++i) {
        multi_thread->schedule([&completed]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            completed.fetch_add(1);
        });
    }
    while (completed.load() < task_count) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto end2 = std::chrono::steady_clock::now();
    auto duration2 = std::chrono::duration_cast<std::chrono::milliseconds>(end2 - start2).count();
    
    // 多线程应该更快（允许一定误差）
    EXPECT_LT(duration2, duration1 * 0.8);
    
    single_thread->stop();
    multi_thread->stop();
}

// ==================== 协程状态管理测试 ====================

// 测试11：协程状态转换跟踪
TEST_F(SchedulerFiberIntegrationTest, FiberStateTracking) {
    std::vector<Fiber::State> states;
    std::mutex states_mutex;
    
    scheduler_->start();
    
    auto fiber = std::make_shared<Fiber>([&states, &states_mutex]() {
        {
            std::lock_guard<std::mutex> lock(states_mutex);
            states.push_back(Fiber::State::kRunning);
        }
        Fiber::yield();
        {
            std::lock_guard<std::mutex> lock(states_mutex);
            states.push_back(Fiber::State::kRunning);
        }
    });
    
    // 初始状态
    EXPECT_EQ(fiber->state(), Fiber::State::kReady);
    
    scheduler_->schedule(fiber);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 验证状态转换
    EXPECT_FALSE(states.empty());
    
    scheduler_->stop();
}

// 测试12：大量协程并发执行
TEST_F(SchedulerFiberIntegrationTest, MassiveConcurrentFibers) {
    const int fiber_count = 5000;
    std::atomic<int> completed{0};
    
    scheduler_->start();
    
    for (int i = 0; i < fiber_count; ++i) {
        scheduler_->schedule([&completed, i]() {
            completed.fetch_add(1);
        });
    }
    
    // 等待所有协程完成
    auto start = std::chrono::steady_clock::now();
    while (completed.load() < fiber_count) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
        if (elapsed > 10) { // 超时保护
            break;
        }
    }
    
    EXPECT_EQ(completed.load(), fiber_count);
    
    scheduler_->stop();
}

// 测试13：调度器重启
TEST_F(SchedulerFiberIntegrationTest, SchedulerRestart) {
    std::atomic<int> count{0};
    
    // 第一次运行
    scheduler_->start();
    scheduler_->schedule([&count]() { count.fetch_add(1); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    scheduler_->stop();
    
    int first_count = count.load();
    EXPECT_GT(first_count, 0);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // 第二次运行
    scheduler_->start();
    scheduler_->schedule([&count]() { count.fetch_add(1); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    EXPECT_GT(count.load(), first_count);
    
    scheduler_->stop();
}

// 测试14：调度器名称和标识
TEST_F(SchedulerFiberIntegrationTest, SchedulerNameAndIdentity) {
    EXPECT_EQ(scheduler_->name(), "TestScheduler");
    
    auto another = std::make_shared<Scheduler>(2, "AnotherScheduler");
    EXPECT_EQ(another->name(), "AnotherScheduler");
    EXPECT_NE(scheduler_->name(), another->name());
}

// 测试15：协程池复用验证
TEST_F(SchedulerFiberIntegrationTest, FiberPoolReuseVerification) {
    scheduler_->start();
    
    // 第一批协程
    for (int i = 0; i < 20; ++i) {
        auto fiber = pool_->get([]() {});
        scheduler_->schedule(fiber);
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    auto stats1 = pool_->get_statistics();
    size_t created_after_first = stats1.total_created;
    
    // 第二批协程（应该复用）
    for (int i = 0; i < 20; ++i) {
        auto fiber = pool_->get([]() {});
        scheduler_->schedule(fiber);
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    auto stats2 = pool_->get_statistics();
    
    // 第二批应该有复用
    EXPECT_GT(stats2.total_reused, 0);
    EXPECT_LE(stats2.total_created, created_after_first + 20);
    
    scheduler_->stop();
}
#include <gtest/gtest.h>
#include <thread>
#include <atomic>
#include <vector>
#include "scheduling/scheduler.h"
#include "scheduling/fiber_pool.h"
#include "runtime/fiber.h"

using namespace zcoroutine;

class SchedulerFiberIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        scheduler_ = std::make_shared<Scheduler>(4, "TestScheduler");
        pool_ = std::make_shared<FiberPool>(10, 100);
        scheduler_->set_fiber_pool(pool_);
    }

    void TearDown() override {
        if (scheduler_ && scheduler_->is_running()) {
            scheduler_->stop();
        }
        scheduler_.reset();
        pool_.reset();
    }

    Scheduler::ptr scheduler_;
    FiberPool::ptr pool_;
};

// ==================== 调度器与协程集成测试 ====================

// 测试1：调度器启动和停止
TEST_F(SchedulerFiberIntegrationTest, StartAndStop) {
    EXPECT_FALSE(scheduler_->is_running());
    
    scheduler_->start();
    EXPECT_TRUE(scheduler_->is_running());
    
    scheduler_->stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(scheduler_->is_running());
}

// 测试2：调度器执行单个协程
TEST_F(SchedulerFiberIntegrationTest, ScheduleSingleFiber) {
    std::atomic<bool> executed{false};
    
    scheduler_->start();
    
    auto fiber = std::make_shared<Fiber>([&executed]() {
        executed.store(true);
    });
    
    scheduler_->schedule(fiber);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(executed.load());
    
    scheduler_->stop();
}

// 测试3：调度器执行多个协程
TEST_F(SchedulerFiberIntegrationTest, ScheduleMultipleFibers) {
    const int fiber_count = 100;
    std::atomic<int> completed{0};
    
    scheduler_->start();
    
    for (int i = 0; i < fiber_count; ++i) {
        auto fiber = std::make_shared<Fiber>([&completed]() {
            completed.fetch_add(1);
        });
        scheduler_->schedule(fiber);
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(completed.load(), fiber_count);
    
    scheduler_->stop();
}

// 测试4：调度器执行回调函数
TEST_F(SchedulerFiberIntegrationTest, ScheduleCallbackFunction) {
    std::atomic<int> sum{0};
    
    scheduler_->start();
    
    for (int i = 1; i <= 10; ++i) {
        scheduler_->schedule([&sum, i]() {
            sum.fetch_add(i);
        });
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(sum.load(), 55); // 1+2+...+10 = 55
    
    scheduler_->stop();
}

// 测试5：协程池与调度器协同工作
TEST_F(SchedulerFiberIntegrationTest, FiberPoolIntegration) {
    std::atomic<int> executed{0};
    
    scheduler_->start();
    
    // 使用协程池创建并调度协程
    for (int i = 0; i < 50; ++i) {
        auto fiber = pool_->get([&executed]() {
            executed.fetch_add(1);
        });
        scheduler_->schedule(fiber);
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(executed.load(), 50);
    
    // 检查协程池统计
    auto stats = pool_->get_statistics();
    EXPECT_GT(stats.total_created, 0);
    
    scheduler_->stop();
}

// 测试6：带yield的协程调度
TEST_F(SchedulerFiberIntegrationTest, FiberWithYield) {
    std::atomic<int> step{0};
    
    scheduler_->start();
    
    auto fiber = std::make_shared<Fiber>([&step]() {
        step.store(1);
        Fiber::yield();
        step.store(2);
        Fiber::yield();
        step.store(3);
    });
    
    scheduler_->schedule(fiber);
    
    // 等待协程完成所有步骤
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(step.load(), 3);
    
    scheduler_->stop();
}

// 测试7：高负载下的调度器
TEST_F(SchedulerFiberIntegrationTest, HighLoadScheduling) {
    const int task_count = 1000;
    std::atomic<int> completed{0};
    
    scheduler_->start();
    
    for (int i = 0; i < task_count; ++i) {
        scheduler_->schedule([&completed, i]() {
            // 模拟一些工作
            int sum = 0;
            for (int j = 0; j < 100; ++j) {
                sum += j;
            }
            completed.fetch_add(1);
        });
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(completed.load(), task_count);
    
    scheduler_->stop();
}

// 测试8：混合协程和回调任务
TEST_F(SchedulerFiberIntegrationTest, MixedFiberAndCallback) {
    std::atomic<int> fiber_count{0};
    std::atomic<int> callback_count{0};
    
    scheduler_->start();
    
    for (int i = 0; i < 50; ++i) {
        if (i % 2 == 0) {
            auto fiber = std::make_shared<Fiber>([&fiber_count]() {
                fiber_count.fetch_add(1);
            });
            scheduler_->schedule(fiber);
        } else {
            scheduler_->schedule([&callback_count]() {
                callback_count.fetch_add(1);
            });
        }
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(fiber_count.load(), 25);
    EXPECT_EQ(callback_count.load(), 25);
    
    scheduler_->stop();
}

// 测试9：协程异常处理
TEST_F(SchedulerFiberIntegrationTest, FiberExceptionHandling) {
    std::atomic<int> completed{0};
    std::atomic<int> failed{0};
    
    scheduler_->start();
    
    for (int i = 0; i < 10; ++i) {
        auto fiber = std::make_shared<Fiber>([&completed, &failed, i]() {
            if (i % 3 == 0) {
                failed.fetch_add(1);
                throw std::runtime_error("Test exception");
            }
            completed.fetch_add(1);
        });
        
        scheduler_->schedule(fiber);
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // 即使有异常，其他协程也应该正常完成
    EXPECT_GT(completed.load(), 0);
    
    scheduler_->stop();
}

// 测试10：调度器线程池大小影响
TEST_F(SchedulerFiberIntegrationTest, ThreadPoolSize) {
    std::atomic<int> completed{0};
    
    // 创建不同线程数的调度器
    auto single_thread = std::make_shared<Scheduler>(1, "Single");
    auto multi_thread = std::make_shared<Scheduler>(4, "Multi");
    
    single_thread->start();
    multi_thread->start();
    
    const int task_count = 100;
    
    // 单线程调度器
    auto start1 = std::chrono::steady_clock::now();
    for (int i = 0; i < task_count; ++i) {
        single_thread->schedule([&completed]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            completed.fetch_add(1);
        });
    }
    while (completed.load() < task_count) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto end1 = std::chrono::steady_clock::now();
    auto duration1 = std::chrono::duration_cast<std::chrono::milliseconds>(end1 - start1).count();
    
    completed.store(0);
    
    // 多线程调度器
    auto start2 = std::chrono::steady_clock::now();
    for (int i = 0; i < task_count; ++i) {
        multi_thread->schedule([&completed]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            completed.fetch_add(1);
        });
    }
    while (completed.load() < task_count) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto end2 = std::chrono::steady_clock::now();
    auto duration2 = std::chrono::duration_cast<std::chrono::milliseconds>(end2 - start2).count();
    
    // 多线程应该更快（允许一定误差）
    EXPECT_LT(duration2, duration1 * 0.8);
    
    single_thread->stop();
    multi_thread->stop();
}

// ==================== 协程状态管理测试 ====================

// 测试11：协程状态转换跟踪
TEST_F(SchedulerFiberIntegrationTest, FiberStateTracking) {
    std::vector<Fiber::State> states;
    std::mutex states_mutex;
    
    scheduler_->start();
    
    auto fiber = std::make_shared<Fiber>([&states, &states_mutex]() {
        {
            std::lock_guard<std::mutex> lock(states_mutex);
            states.push_back(Fiber::State::kRunning);
        }
        Fiber::yield();
        {
            std::lock_guard<std::mutex> lock(states_mutex);
            states.push_back(Fiber::State::kRunning);
        }
    });
    
    // 初始状态
    EXPECT_EQ(fiber->state(), Fiber::State::kReady);
    
    scheduler_->schedule(fiber);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 验证状态转换
    EXPECT_FALSE(states.empty());
    
    scheduler_->stop();
}

// 测试12：大量协程并发执行
TEST_F(SchedulerFiberIntegrationTest, MassiveConcurrentFibers) {
    const int fiber_count = 5000;
    std::atomic<int> completed{0};
    
    scheduler_->start();
    
    for (int i = 0; i < fiber_count; ++i) {
        scheduler_->schedule([&completed, i]() {
            completed.fetch_add(1);
        });
    }
    
    // 等待所有协程完成
    auto start = std::chrono::steady_clock::now();
    while (completed.load() < fiber_count) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
        if (elapsed > 10) { // 超时保护
            break;
        }
    }
    
    EXPECT_EQ(completed.load(), fiber_count);
    
    scheduler_->stop();
}

// 测试13：调度器重启
TEST_F(SchedulerFiberIntegrationTest, SchedulerRestart) {
    std::atomic<int> count{0};
    
    // 第一次运行
    scheduler_->start();
    scheduler_->schedule([&count]() { count.fetch_add(1); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    scheduler_->stop();
    
    int first_count = count.load();
    EXPECT_GT(first_count, 0);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // 第二次运行
    scheduler_->start();
    scheduler_->schedule([&count]() { count.fetch_add(1); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    EXPECT_GT(count.load(), first_count);
    
    scheduler_->stop();
}

// 测试14：调度器名称和标识
TEST_F(SchedulerFiberIntegrationTest, SchedulerNameAndIdentity) {
    EXPECT_EQ(scheduler_->name(), "TestScheduler");
    
    auto another = std::make_shared<Scheduler>(2, "AnotherScheduler");
    EXPECT_EQ(another->name(), "AnotherScheduler");
    EXPECT_NE(scheduler_->name(), another->name());
}

// 测试15：协程池复用验证
TEST_F(SchedulerFiberIntegrationTest, FiberPoolReuseVerification) {
    scheduler_->start();
    
    // 第一批协程
    for (int i = 0; i < 20; ++i) {
        auto fiber = pool_->get([]() {});
        scheduler_->schedule(fiber);
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    auto stats1 = pool_->get_statistics();
    size_t created_after_first = stats1.total_created;
    
    // 第二批协程（应该复用）
    for (int i = 0; i < 20; ++i) {
        auto fiber = pool_->get([]() {});
        scheduler_->schedule(fiber);
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    auto stats2 = pool_->get_statistics();
    
    // 第二批应该有复用
    EXPECT_GT(stats2.total_reused, 0);
    EXPECT_LE(stats2.total_created, created_after_first + 20);
    
    scheduler_->stop();
}
