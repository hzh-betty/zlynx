#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <thread>
#include <vector>
#include <atomic>
#include "scheduling/fiber_pool.h"
#include "runtime/fiber.h"

using namespace zcoroutine;

class FiberPoolTest : public ::testing::Test {
protected:
    void SetUp() override {
        pool_ = std::make_shared<FiberPool>(5, 50);
    }

    void TearDown() override {
        pool_->clear();
        pool_.reset();
    }

    FiberPool::ptr pool_;
};

// ==================== 基础功能测试 ====================

// 测试1：创建协程池
TEST_F(FiberPoolTest, CreatePool) {
    ASSERT_NE(pool_, nullptr);
    EXPECT_EQ(pool_->get_idle_count(), 0);
    auto stats = pool_->get_statistics();
    EXPECT_EQ(stats.total_created, 0);
    EXPECT_EQ(stats.total_reused, 0);
}

// 测试2：从池中获取协程
TEST_F(FiberPoolTest, GetFiber) {
    bool executed = false;
    auto fiber = pool_->get([&executed]() {
        executed = true;
    });
    
    ASSERT_NE(fiber, nullptr);
    EXPECT_EQ(fiber->state(), Fiber::State::kReady);
    
    fiber->resume();
    EXPECT_TRUE(executed);
    EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
}

// 测试3：归还协程到池
TEST_F(FiberPoolTest, ReleaseFiber) {
    auto fiber = pool_->get([]() {});
    fiber->resume();
    
    EXPECT_EQ(fiber->state(), Fiber::State::kTerminated);
    EXPECT_EQ(pool_->get_idle_count(), 0);
    
    pool_->release(fiber);
    EXPECT_EQ(pool_->get_idle_count(), 1);
}

// 测试4：协程复用
TEST_F(FiberPoolTest, FiberReuse) {
    int count1 = 0;
    int count2 = 0;
    
    // 第一次使用
    auto fiber1 = pool_->get([&count1]() { count1++; });
    auto fiber1_id = fiber1->id();
    fiber1->resume();
    pool_->release(fiber1);
    
    // 第二次使用（应该复用）
    auto fiber2 = pool_->get([&count2]() { count2++; });
    auto fiber2_id = fiber2->id();
    fiber2->resume();
    
    EXPECT_EQ(count1, 1);
    EXPECT_EQ(count2, 1);
    EXPECT_EQ(fiber1_id, fiber2_id); // 复用同一个协程对象
    
    auto stats = pool_->get_statistics();
    EXPECT_EQ(stats.total_created, 1);
    EXPECT_EQ(stats.total_reused, 1);
}

// 测试5：多个协程的获取和归还
TEST_F(FiberPoolTest, MultipleGetAndRelease) {
    std::vector<Fiber::ptr> fibers;
    
    // 获取10个协程
    for (int i = 0; i < 10; ++i) {
        auto fiber = pool_->get([i]() {});
        fiber->resume();
        fibers.push_back(fiber);
    }
    
    EXPECT_EQ(pool_->get_idle_count(), 0);
    
    // 归还所有协程
    for (auto& fiber : fibers) {
        pool_->release(fiber);
    }
    
    EXPECT_EQ(pool_->get_idle_count(), 10);
}

// ==================== 边界条件测试 ====================

// 测试6：空池获取协程
TEST_F(FiberPoolTest, GetFromEmptyPool) {
    EXPECT_EQ(pool_->get_idle_count(), 0);
    
    auto fiber = pool_->get([]() {});
    ASSERT_NE(fiber, nullptr);
    
    auto stats = pool_->get_statistics();
    EXPECT_EQ(stats.total_created, 1);
    EXPECT_EQ(stats.total_reused, 0);
}

// 测试7：归还未终止的协程（应该失败或忽略）
TEST_F(FiberPoolTest, ReleaseNonTerminatedFiber) {
    auto fiber = pool_->get([]() {
        Fiber::yield();
    });
    
    fiber->resume();
    EXPECT_EQ(fiber->state(), Fiber::State::kSuspended);
    
    size_t before_count = pool_->get_idle_count();
    pool_->release(fiber); // 应该被忽略
    size_t after_count = pool_->get_idle_count();
    
    // 未终止的协程不应该被归还
    EXPECT_EQ(before_count, after_count);
}

// 测试8：归还nullptr（应该安全处理）
TEST_F(FiberPoolTest, ReleaseNullptr) {
    EXPECT_NO_THROW({
        pool_->release(nullptr);
    });
}

// 测试9：清空池
TEST_F(FiberPoolTest, ClearPool) {
    // 添加多个空闲协程
    for (int i = 0; i < 5; ++i) {
        auto fiber = pool_->get([]() {});
        fiber->resume();
        pool_->release(fiber);
    }
    
    EXPECT_EQ(pool_->get_idle_count(), 5);
    
    pool_->clear();
    EXPECT_EQ(pool_->get_idle_count(), 0);
}

// 测试10：调整池大小
TEST_F(FiberPoolTest, ResizePool) {
    // 添加多个空闲协程
    for (int i = 0; i < 10; ++i) {
        auto fiber = pool_->get([]() {});
        fiber->resume();
        pool_->release(fiber);
    }
    
    EXPECT_EQ(pool_->get_idle_count(), 10);
    
    // 调整为更小的大小
    pool_->resize(5);
    EXPECT_LE(pool_->get_idle_count(), 5);
}

// ==================== 统计信息测试 ====================

// 测试11：统计协程创建数
TEST_F(FiberPoolTest, StatisticsCreated) {
    for (int i = 0; i < 10; ++i) {
        auto fiber = pool_->get([i]() {});
        fiber->resume();
    }
    
    auto stats = pool_->get_statistics();
    EXPECT_EQ(stats.total_created, 10);
}

// 测试12：统计协程复用数
TEST_F(FiberPoolTest, StatisticsReused) {
    std::vector<Fiber::ptr> fibers;
    
    // 创建5个协程
    for (int i = 0; i < 5; ++i) {
        auto fiber = pool_->get([i]() {});
        fiber->resume();
        fibers.push_back(fiber);
    }
    
    // 归还
    for (auto& fiber : fibers) {
        pool_->release(fiber);
    }
    fibers.clear();
    
    // 再次获取（复用）
    for (int i = 0; i < 5; ++i) {
        auto fiber = pool_->get([i]() {});
        fiber->resume();
    }
    
    auto stats = pool_->get_statistics();
    EXPECT_EQ(stats.total_created, 5);
    EXPECT_EQ(stats.total_reused, 5);
}

// 测试13：统计空闲协程数
TEST_F(FiberPoolTest, StatisticsIdleCount) {
    std::vector<Fiber::ptr> fibers;
    
    for (int i = 0; i < 5; ++i) {
        auto fiber = pool_->get([i]() {});
        fiber->resume();
        fibers.push_back(fiber);
    }
    
    // 逐个归还，检查空闲数
    for (size_t i = 0; i < fibers.size(); ++i) {
        pool_->release(fibers[i]);
        EXPECT_EQ(pool_->get_idle_count(), i + 1);
    }
}

// ==================== 并发安全测试 ====================

// 测试14：并发获取协程
TEST_F(FiberPoolTest, ConcurrentGet) {
    const int thread_num = 10;
    const int fibers_per_thread = 10;
    std::vector<std::thread> threads;
    std::atomic<int> total_executed{0};
    
    for (int t = 0; t < thread_num; ++t) {
        threads.emplace_back([this, &total_executed, fibers_per_thread]() {
            for (int i = 0; i < fibers_per_thread; ++i) {
                auto fiber = pool_->get([&total_executed]() {
                    total_executed.fetch_add(1);
                });
                ASSERT_NE(fiber, nullptr);
                fiber->resume();
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(total_executed.load(), thread_num * fibers_per_thread);
}

// 测试15：并发归还协程
TEST_F(FiberPoolTest, ConcurrentRelease) {
    const int thread_num = 10;
    const int fibers_per_thread = 10;
    std::vector<std::thread> threads;
    std::vector<std::vector<Fiber::ptr>> thread_fibers(thread_num);
    
    // 先创建协程
    for (int t = 0; t < thread_num; ++t) {
        for (int i = 0; i < fibers_per_thread; ++i) {
            auto fiber = pool_->get([]() {});
            fiber->resume();
            thread_fibers[t].push_back(fiber);
        }
    }
    
    // 并发归还
    for (int t = 0; t < thread_num; ++t) {
        threads.emplace_back([this, &thread_fibers, t]() {
            for (auto& fiber : thread_fibers[t]) {
                pool_->release(fiber);
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(pool_->get_idle_count(), thread_num * fibers_per_thread);
}

// 测试16：并发获取和归还
TEST_F(FiberPoolTest, ConcurrentGetAndRelease) {
    const int thread_num = 8;
    const int operations = 100;
    std::vector<std::thread> threads;
    std::atomic<int> total_ops{0};
    
    for (int t = 0; t < thread_num; ++t) {
        threads.emplace_back([this, &total_ops, operations]() {
            for (int i = 0; i < operations; ++i) {
                auto fiber = pool_->get([&total_ops]() {
                    total_ops.fetch_add(1);
                });
                fiber->resume();
                pool_->release(fiber);
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(total_ops.load(), thread_num * operations);
    
    // 最终所有协程都应该被归还
    auto stats = pool_->get_statistics();
    EXPECT_GT(stats.total_reused, 0); // 应该有复用
}

// ==================== 容量限制测试 ====================

// 测试17：最小容量限制
TEST_F(FiberPoolTest, MinSizeConstraint) {
    auto small_pool = std::make_shared<FiberPool>(0, 10);
    
    auto fiber = small_pool->get([]() {});
    ASSERT_NE(fiber, nullptr);
    fiber->resume();
    small_pool->release(fiber);
    
    EXPECT_GE(small_pool->get_idle_count(), 0);
}

// 测试18：最大容量限制
TEST_F(FiberPoolTest, MaxSizeConstraint) {
    auto limited_pool = std::make_shared<FiberPool>(1, 5);
    std::vector<Fiber::ptr> fibers;
    
    // 创建超过最大容量的协程
    for (int i = 0; i < 10; ++i) {
        auto fiber = limited_pool->get([i]() {});
        fiber->resume();
        fibers.push_back(fiber);
    }
    
    // 归还所有协程
    for (auto& fiber : fibers) {
        limited_pool->release(fiber);
    }
    
    // 空闲协程数不应超过最大容量
    EXPECT_LE(limited_pool->get_idle_count(), 5);
}

// ==================== 协程重置测试 ====================

// 测试19：协程重置后复用
TEST_F(FiberPoolTest, FiberResetAndReuse) {
    int first_count = 0;
    int second_count = 0;
    
    auto fiber = pool_->get([&first_count]() {
        first_count++;
    });
    fiber->resume();
    EXPECT_EQ(first_count, 1);
    
    pool_->release(fiber);
    
    // 再次获取（应该复用并重置）
    auto reused = pool_->get([&second_count]() {
        second_count++;
    });
    reused->resume();
    
    EXPECT_EQ(second_count, 1);
    EXPECT_EQ(first_count, 1); // 第一个计数器不应该改变
}

// 测试20：多次重置复用
TEST_F(FiberPoolTest, MultipleResetAndReuse) {
    std::vector<int> counts(5, 0);
    
    auto fiber = pool_->get([&counts]() { counts[0]++; });
    fiber->resume();
    pool_->release(fiber);
    
    for (int i = 1; i < 5; ++i) {
        auto reused = pool_->get([&counts, i]() { counts[i]++; });
        reused->resume();
        pool_->release(reused);
    }
    
    // 每个计数器都应该是1
    for (int count : counts) {
        EXPECT_EQ(count, 1);
    }
    
    auto stats = pool_->get_statistics();
    EXPECT_EQ(stats.total_created, 1);
    EXPECT_EQ(stats.total_reused, 4);
}

// ==================== 异常处理测试 ====================

// 测试21：协程内部异常不影响池
TEST_F(FiberPoolTest, ExceptionInFiber) {
    auto fiber = pool_->get([]() {
        throw std::runtime_error("Test exception");
    });
    
    EXPECT_THROW({
        fiber->resume();
    }, std::runtime_error);
    
    // 池应该仍然可用
    auto another = pool_->get([]() {});
    ASSERT_NE(another, nullptr);
    another->resume();
}

// 测试22：多个协程异常
TEST_F(FiberPoolTest, MultipleExceptions) {
    const int count = 5;
    int exception_count = 0;
    
    for (int i = 0; i < count; ++i) {
        auto fiber = pool_->get([i]() {
            if (i % 2 == 0) {
                throw std::runtime_error("Exception");
            }
        });
        
        try {
            fiber->resume();
        } catch (...) {
            exception_count++;
        }
    }
    
    EXPECT_EQ(exception_count, 3); // 0, 2, 4抛出异常
    
    // 池仍然可用
    auto normal = pool_->get([]() {});
    ASSERT_NE(normal, nullptr);
}

// ==================== 性能相关测试 ====================

// 测试23：大量协程创建
TEST_F(FiberPoolTest, MassiveCreation) {
    const int count = 1000;
    std::vector<Fiber::ptr> fibers;
    
    for (int i = 0; i < count; ++i) {
        auto fiber = pool_->get([i]() {});
        fibers.push_back(fiber);
    }
    
    EXPECT_EQ(fibers.size(), count);
    
    for (auto& fiber : fibers) {
        fiber->resume();
    }
}

// 测试24：大量协程复用
TEST_F(FiberPoolTest, MassiveReuse) {
    const int rounds = 100;
    const int batch_size = 10;
    
    for (int r = 0; r < rounds; ++r) {
        std::vector<Fiber::ptr> batch;
        
        for (int i = 0; i < batch_size; ++i) {
            auto fiber = pool_->get([r, i]() {});
            fiber->resume();
            batch.push_back(fiber);
        }
        
        for (auto& fiber : batch) {
            pool_->release(fiber);
        }
    }
    
    auto stats = pool_->get_statistics();
    EXPECT_LE(stats.total_created, batch_size);
    EXPECT_GE(stats.total_reused, rounds * batch_size - batch_size);
}

// 测试25：空闲协程清理
TEST_F(FiberPoolTest, IdleFiberCleanup) {
    // 创建大量协程
    for (int i = 0; i < 20; ++i) {
        auto fiber = pool_->get([i]() {});
        fiber->resume();
        pool_->release(fiber);
    }
    
    size_t before = pool_->get_idle_count();
    EXPECT_GT(before, 0);
    
    // 调整大小触发清理
    pool_->resize(5);
    
    size_t after = pool_->get_idle_count();
    EXPECT_LE(after, 5);
}
