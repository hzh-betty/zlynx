#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include "timer/timer.h"
#include "timer/timer_manager.h"

using namespace zcoroutine;

// 辅助函数：获取当前时间（毫秒）
static uint64_t get_current_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

class TimerTest : public ::testing::Test {
protected:
    void SetUp() override {
        manager_ = std::make_unique<TimerManager>();
    }

    void TearDown() override {
        manager_.reset();
    }

    std::unique_ptr<TimerManager> manager_;
};

// ==================== Timer 基础功能测试 ====================

// 测试1：创建一次性定时器
TEST_F(TimerTest, CreateOneShotTimer) {
    bool executed = false;
    auto timer = std::make_shared<Timer>(100, [&executed]() {
        executed = true;
    }, false);
    
    ASSERT_NE(timer, nullptr);
    EXPECT_FALSE(timer->is_recurring());
    EXPECT_GT(timer->get_next_time(), 0);
}

// 测试2：创建循环定时器
TEST_F(TimerTest, CreateRecurringTimer) {
    int count = 0;
    auto timer = std::make_shared<Timer>(100, [&count]() {
        count++;
    }, true);
    
    ASSERT_NE(timer, nullptr);
    EXPECT_TRUE(timer->is_recurring());
}

// 测试3：执行一次性定时器
TEST_F(TimerTest, ExecuteOneShotTimer) {
    bool executed = false;
    auto timer = std::make_shared<Timer>(100, [&executed]() {
        executed = true;
    }, false);
    
    EXPECT_FALSE(executed);
    timer->execute();
    EXPECT_TRUE(executed);
}

// 测试4：执行循环定时器多次
TEST_F(TimerTest, ExecuteRecurringTimer) {
    int count = 0;
    auto timer = std::make_shared<Timer>(100, [&count]() {
        count++;
    }, true);
    
    for (int i = 0; i < 5; ++i) {
        timer->execute();
    }
    
    EXPECT_EQ(count, 5);
}

// 测试5：取消定时器
TEST_F(TimerTest, CancelTimer) {
    bool executed = false;
    auto timer = std::make_shared<Timer>(100, [&executed]() {
        executed = true;
    }, false);
    
    bool result = timer->cancel();
    EXPECT_TRUE(result);
    
    timer->execute();
    EXPECT_FALSE(executed); // 取消后不应执行
}

// 测试6：重复取消定时器
TEST_F(TimerTest, DoubleCancelTimer) {
    auto timer = std::make_shared<Timer>(100, []() {}, false);
    
    EXPECT_TRUE(timer->cancel());
    EXPECT_FALSE(timer->cancel()); // 第二次取消应该返回false
}

// 测试7：刷新定时器
TEST_F(TimerTest, RefreshTimer) {
    auto timer = std::make_shared<Timer>(100, []() {}, false);
    
    uint64_t original_time = timer->get_next_time();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    timer->refresh();
    uint64_t new_time = timer->get_next_time();
    
    EXPECT_GT(new_time, original_time);
}

// 测试8：重置定时器超时时间
TEST_F(TimerTest, ResetTimerTimeout) {
    auto timer = std::make_shared<Timer>(100, []() {}, false);
    
    uint64_t before = timer->get_next_time();
    timer->reset(200);
    uint64_t after = timer->get_next_time();
    
    // 新的超时时间应该更晚
    EXPECT_GT(after, before);
}

// ==================== TimerManager 基础功能测试 ====================

// 测试9：添加定时器到管理器
TEST_F(TimerTest, AddTimerToManager) {
    bool executed = false;
    auto timer = manager_->add_timer(100, [&executed]() {
        executed = true;
    }, false);
    
    ASSERT_NE(timer, nullptr);
    EXPECT_FALSE(timer->is_recurring());
}

// 测试10：获取下一个超时时间
TEST_F(TimerTest, GetNextTimeout) {
    // 空管理器应该返回-1
    EXPECT_EQ(manager_->get_next_timeout(), -1);
    
    manager_->add_timer(100, []() {}, false);
    
    int timeout = manager_->get_next_timeout();
    EXPECT_GE(timeout, 0);
    EXPECT_LE(timeout, 100);
}

// 测试11：多个定时器按时间排序
TEST_F(TimerTest, MultipleTimersSorted) {
    manager_->add_timer(300, []() {}, false);
    manager_->add_timer(100, []() {}, false);
    manager_->add_timer(200, []() {}, false);
    
    int timeout = manager_->get_next_timeout();
    EXPECT_LE(timeout, 100); // 最早的定时器应该是100ms
}

// 测试12：列出过期定时器
TEST_F(TimerTest, ListExpiredCallbacks) {
    std::atomic<int> count{0};
    
    manager_->add_timer(10, [&count]() { count++; }, false);
    manager_->add_timer(20, [&count]() { count++; }, false);
    manager_->add_timer(1000, [&count]() { count++; }, false);
    
    // 等待前两个定时器过期
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    auto callbacks = manager_->list_expired_callbacks();
    
    // 执行所有过期的回调
    for (auto& cb : callbacks) {
        cb();
    }
    
    EXPECT_EQ(count.load(), 2); // 只有前两个定时器应该过期
}

// ==================== 循环定时器测试 ====================

// 测试13：循环定时器自动重置
TEST_F(TimerTest, RecurringTimerAutoReset) {
    int count = 0;
    auto timer = manager_->add_timer(50, [&count]() {
        count++;
    }, true);
    
    EXPECT_TRUE(timer->is_recurring());
    
    // 等待并执行多次
    for (int i = 0; i < 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        auto callbacks = manager_->list_expired_callbacks();
        for (auto& cb : callbacks) {
            cb();
        }
    }
    
    EXPECT_GE(count, 3);
}

// 测试14：取消循环定时器
TEST_F(TimerTest, CancelRecurringTimer) {
    int count = 0;
    auto timer = manager_->add_timer(50, [&count]() {
        count++;
    }, true);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    auto callbacks = manager_->list_expired_callbacks();
    for (auto& cb : callbacks) {
        cb();
    }
    
    int count_before_cancel = count;
    timer->cancel();
    
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    callbacks = manager_->list_expired_callbacks();
    for (auto& cb : callbacks) {
        cb();
    }
    
    EXPECT_EQ(count, count_before_cancel); // 取消后不应再执行
}

// ==================== 边界条件测试 ====================

// 测试15：0毫秒超时
TEST_F(TimerTest, ZeroTimeout) {
    bool executed = false;
    auto timer = manager_->add_timer(0, [&executed]() {
        executed = true;
    }, false);
    
    auto callbacks = manager_->list_expired_callbacks();
    for (auto& cb : callbacks) {
        cb();
    }
    
    EXPECT_TRUE(executed);
}

// 测试16：非常大的超时时间
TEST_F(TimerTest, VeryLargeTimeout) {
    auto timer = manager_->add_timer(UINT64_MAX / 2, []() {}, false);
    
    ASSERT_NE(timer, nullptr);
    EXPECT_GT(timer->get_next_time(), get_current_ms());
}

// 测试17：空回调函数
TEST_F(TimerTest, NullCallback) {
    auto timer = manager_->add_timer(100, nullptr, false);
    
    EXPECT_NO_THROW({
        timer->execute();
    });
}

// 测试18：重置为0超时
TEST_F(TimerTest, ResetToZeroTimeout) {
    auto timer = manager_->add_timer(1000, []() {}, false);
    
    timer->reset(0);
    
    auto callbacks = manager_->list_expired_callbacks();
    EXPECT_FALSE(callbacks.empty());
}

// ==================== 并发测试 ====================

// 测试19：并发添加定时器
TEST_F(TimerTest, ConcurrentAddTimer) {
    const int thread_num = 10;
    const int timers_per_thread = 10;
    std::atomic<int> total_count{0};
    std::vector<std::thread> threads;
    
    for (int t = 0; t < thread_num; ++t) {
        threads.emplace_back([this, &total_count, timers_per_thread]() {
            for (int i = 0; i < timers_per_thread; ++i) {
                manager_->add_timer(100, [&total_count]() {
                    total_count++;
                }, false);
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    // 等待所有定时器过期
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    
    auto callbacks = manager_->list_expired_callbacks();
    EXPECT_EQ(callbacks.size(), thread_num * timers_per_thread);
}

// 测试20：并发取消定时器
TEST_F(TimerTest, ConcurrentCancelTimer) {
    std::vector<Timer::ptr> timers;
    
    for (int i = 0; i < 20; ++i) {
        timers.push_back(manager_->add_timer(1000, []() {}, false));
    }
    
    std::vector<std::thread> threads;
    for (int i = 0; i < 20; ++i) {
        threads.emplace_back([&timers, i]() {
            timers[i]->cancel();
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    // 所有定时器都应该被取消
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    auto callbacks = manager_->list_expired_callbacks();
    EXPECT_TRUE(callbacks.empty());
}

// ==================== 精度测试 ====================

// 测试21：定时器精度测试
TEST_F(TimerTest, TimerAccuracy) {
    const uint64_t timeout_ms = 100;
    uint64_t start_time = 0;
    uint64_t end_time = 0;
    
    start_time = get_current_ms();
    manager_->add_timer(timeout_ms, [&end_time]() {
        end_time = get_current_ms();
    }, false);
    
    // 等待定时器过期
    while (end_time == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto callbacks = manager_->list_expired_callbacks();
        for (auto& cb : callbacks) {
            cb();
        }
    }
    
    uint64_t elapsed = end_time - start_time;
    
    // 允许10ms的误差
    EXPECT_GE(elapsed, timeout_ms - 10);
    EXPECT_LE(elapsed, timeout_ms + 50);
}

// 测试22：多个定时器执行顺序
TEST_F(TimerTest, TimerExecutionOrder) {
    std::vector<int> execution_order;
    std::mutex mutex;
    
    manager_->add_timer(50, [&execution_order, &mutex]() {
        std::lock_guard<std::mutex> lock(mutex);
        execution_order.push_back(1);
    }, false);
    
    manager_->add_timer(100, [&execution_order, &mutex]() {
        std::lock_guard<std::mutex> lock(mutex);
        execution_order.push_back(2);
    }, false);
    
    manager_->add_timer(30, [&execution_order, &mutex]() {
        std::lock_guard<std::mutex> lock(mutex);
        execution_order.push_back(0);
    }, false);
    
    // 等待并执行所有定时器
    for (int i = 0; i < 5; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        auto callbacks = manager_->list_expired_callbacks();
        for (auto& cb : callbacks) {
            cb();
        }
    }
    
    ASSERT_EQ(execution_order.size(), 3);
    EXPECT_EQ(execution_order[0], 0);
    EXPECT_EQ(execution_order[1], 1);
    EXPECT_EQ(execution_order[2], 2);
}

// ==================== 复杂场景测试 ====================

// 测试23：大量定时器
TEST_F(TimerTest, MassiveTimers) {
    const int timer_count = 1000;
    std::atomic<int> executed{0};
    
    for (int i = 0; i < timer_count; ++i) {
        manager_->add_timer(50 + (i % 100), [&executed]() {
            executed++;
        }, false);
    }
    
    // 等待所有定时器过期
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    int total = 0;
    while (true) {
        auto callbacks = manager_->list_expired_callbacks();
        if (callbacks.empty()) break;
        
        for (auto& cb : callbacks) {
            cb();
        }
        total += callbacks.size();
        
        if (total >= timer_count) break;
    }
    
    EXPECT_EQ(executed.load(), timer_count);
}

// 测试24：混合一次性和循环定时器
TEST_F(TimerTest, MixedTimers) {
    std::atomic<int> oneshot_count{0};
    std::atomic<int> recurring_count{0};
    
    // 添加一次性定时器
    for (int i = 0; i < 5; ++i) {
        manager_->add_timer(50, [&oneshot_count]() {
            oneshot_count++;
        }, false);
    }
    
    // 添加循环定时器
    for (int i = 0; i < 3; ++i) {
        manager_->add_timer(50, [&recurring_count]() {
            recurring_count++;
        }, true);
    }
    
    // 执行多轮
    for (int round = 0; round < 3; ++round) {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        auto callbacks = manager_->list_expired_callbacks();
        for (auto& cb : callbacks) {
            cb();
        }
    }
    
    EXPECT_EQ(oneshot_count.load(), 5);
    EXPECT_GE(recurring_count.load(), 6); // 至少执行2轮
}

// 测试25：定时器刷新后重新排序
TEST_F(TimerTest, TimerRefreshReorder) {
    bool first_executed = false;
    bool second_executed = false;
    
    auto timer1 = manager_->add_timer(50, [&first_executed]() {
        first_executed = true;
    }, false);
    
    auto timer2 = manager_->add_timer(100, [&second_executed]() {
        second_executed = true;
    }, false);
    
    // 刷新第一个定时器，使它延后
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    timer1->reset(200);
    
    // 现在第二个定时器应该先过期
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    auto callbacks = manager_->list_expired_callbacks();
    for (auto& cb : callbacks) {
        cb();
    }
    
    EXPECT_TRUE(second_executed);
    EXPECT_FALSE(first_executed); // 第一个定时器被延后，还没过期
}
