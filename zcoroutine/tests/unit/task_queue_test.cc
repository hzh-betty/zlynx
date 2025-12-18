#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include "scheduling/task_queue.h"
#include "runtime/fiber.h"

using namespace zcoroutine;

class TaskQueueTest : public ::testing::Test {
protected:
    void SetUp() override {
        queue_ = std::make_unique<TaskQueue>();
    }

    void TearDown() override {
        queue_->stop();
        queue_.reset();
    }

    std::unique_ptr<TaskQueue> queue_;
};

// ==================== 基础功能测试 ====================

// 测试1：创建任务队列
TEST_F(TaskQueueTest, CreateQueue) {
    ASSERT_NE(queue_, nullptr);
    EXPECT_TRUE(queue_->empty());
    EXPECT_EQ(queue_->size(), 0);
}

// 测试2：添加回调任务
TEST_F(TaskQueueTest, PushCallbackTask) {
    bool executed = false;
    Task task([&executed]() { executed = true; });
    
    queue_->push(task);
    
    EXPECT_FALSE(queue_->empty());
    EXPECT_EQ(queue_->size(), 1);
    
    Task popped;
    EXPECT_TRUE(queue_->pop(popped));
    EXPECT_TRUE(popped.is_valid());
    EXPECT_NE(popped.callback, nullptr);
    
    popped.callback();
    EXPECT_TRUE(executed);
}

// 测试3：添加协程任务
TEST_F(TaskQueueTest, PushFiberTask) {
    bool executed = false;
    auto fiber = std::make_shared<Fiber>([&executed]() { 
        executed = true; 
    });
    
    Task task(fiber);
    queue_->push(task);
    
    EXPECT_EQ(queue_->size(), 1);
    
    Task popped;
    EXPECT_TRUE(queue_->pop(popped));
    EXPECT_TRUE(popped.is_valid());
    EXPECT_NE(popped.fiber, nullptr);
    
    popped.fiber->resume();
    EXPECT_TRUE(executed);
}

// 测试4：阻塞pop
TEST_F(TaskQueueTest, BlockingPop) {
    std::atomic<bool> task_popped{false};
    
    std::thread consumer([this, &task_popped]() {
        Task task;
        bool success = queue_->pop(task);
        task_popped.store(success);
    });
    
    // 等待消费者线程开始等待
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // 添加任务
    queue_->push(Task([]() {}));
    
    consumer.join();
    EXPECT_TRUE(task_popped.load());
}

// 测试5：非阻塞try_pop
TEST_F(TaskQueueTest, NonBlockingTryPop) {
    Task task;
    
    // 队列为空时应该返回false
    EXPECT_FALSE(queue_->try_pop(task));
    
    // 添加任务后应该返回true
    queue_->push(Task([]() {}));
    EXPECT_TRUE(queue_->try_pop(task));
    EXPECT_TRUE(task.is_valid());
}

// 测试6：多个任务的FIFO顺序
TEST_F(TaskQueueTest, FIFOOrder) {
    std::vector<int> results;
    
    for (int i = 0; i < 5; ++i) {
        queue_->push(Task([&results, i]() { results.push_back(i); }));
    }
    
    EXPECT_EQ(queue_->size(), 5);
    
    while (!queue_->empty()) {
        Task task;
        queue_->pop(task);
        task.callback();
    }
    
    // 验证FIFO顺序
    ASSERT_EQ(results.size(), 5);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(results[i], i);
    }
}

// ==================== 停止队列测试 ====================

// 测试7：停止队列
TEST_F(TaskQueueTest, StopQueue) {
    queue_->push(Task([]() {}));
    EXPECT_FALSE(queue_->empty());
    
    queue_->stop();
    
    Task task;
    EXPECT_FALSE(queue_->pop(task)); // 停止后pop应该返回false
}

// 测试8：停止后唤醒等待线程
TEST_F(TaskQueueTest, StopWakesWaitingThreads) {
    std::atomic<int> woken_threads{0};
    std::vector<std::thread> threads;
    
    // 启动多个等待线程
    for (int i = 0; i < 5; ++i) {
        threads.emplace_back([this, &woken_threads]() {
            Task task;
            bool result = queue_->pop(task);
            if (!result) {
                woken_threads.fetch_add(1);
            }
        });
    }
    
    // 等待线程开始等待
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // 停止队列
    queue_->stop();
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(woken_threads.load(), 5);
}

// 测试9：停止后无法添加任务（或添加后无效）
TEST_F(TaskQueueTest, CannotPushAfterStop) {
    queue_->stop();
    
    queue_->push(Task([]() {}));
    
    Task task;
    EXPECT_FALSE(queue_->pop(task));
}

// ==================== 并发测试 ====================

// 测试10：多生产者单消费者
TEST_F(TaskQueueTest, MultiProducerSingleConsumer) {
    const int producer_num = 5;
    const int tasks_per_producer = 100;
    std::atomic<int> consumed{0};
    std::vector<std::thread> producers;
    
    // 启动消费者
    std::thread consumer([this, &consumed]() {
        while (true) {
            Task task;
            if (!queue_->pop(task)) {
                break;
            }
            if (task.is_valid() && task.callback) {
                task.callback();
                consumed.fetch_add(1);
            }
        }
    });
    
    // 启动生产者
    for (int p = 0; p < producer_num; ++p) {
        producers.emplace_back([this, tasks_per_producer]() {
            for (int i = 0; i < tasks_per_producer; ++i) {
                queue_->push(Task([]() {}));
            }
        });
    }
    
    // 等待所有生产者完成
    for (auto& t : producers) {
        t.join();
    }
    
    // 停止队列并等待消费者
    queue_->stop();
    consumer.join();
    
    EXPECT_EQ(consumed.load(), producer_num * tasks_per_producer);
}

// 测试11：单生产者多消费者
TEST_F(TaskQueueTest, SingleProducerMultiConsumer) {
    const int consumer_num = 5;
    const int total_tasks = 500;
    std::atomic<int> consumed{0};
    std::vector<std::thread> consumers;
    
    // 启动消费者
    for (int c = 0; c < consumer_num; ++c) {
        consumers.emplace_back([this, &consumed]() {
            while (true) {
                Task task;
                if (!queue_->pop(task)) {
                    break;
                }
                if (task.is_valid()) {
                    consumed.fetch_add(1);
                }
            }
        });
    }
    
    // 生产任务
    for (int i = 0; i < total_tasks; ++i) {
        queue_->push(Task([]() {}));
    }
    
    // 停止队列并等待消费者
    queue_->stop();
    for (auto& t : consumers) {
        t.join();
    }
    
    EXPECT_EQ(consumed.load(), total_tasks);
}

// 测试12：多生产者多消费者
TEST_F(TaskQueueTest, MultiProducerMultiConsumer) {
    const int producer_num = 4;
    const int consumer_num = 4;
    const int tasks_per_producer = 100;
    std::atomic<int> produced{0};
    std::atomic<int> consumed{0};
    std::vector<std::thread> threads;
    
    // 启动消费者
    for (int c = 0; c < consumer_num; ++c) {
        threads.emplace_back([this, &consumed]() {
            while (true) {
                Task task;
                if (!queue_->pop(task)) {
                    break;
                }
                if (task.is_valid()) {
                    consumed.fetch_add(1);
                }
            }
        });
    }
    
    // 启动生产者
    for (int p = 0; p < producer_num; ++p) {
        threads.emplace_back([this, &produced, tasks_per_producer]() {
            for (int i = 0; i < tasks_per_producer; ++i) {
                queue_->push(Task([]() {}));
                produced.fetch_add(1);
            }
        });
    }
    
    // 等待生产完成
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // 停止队列
    queue_->stop();
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(produced.load(), producer_num * tasks_per_producer);
    EXPECT_EQ(consumed.load(), produced.load());
}

// ==================== 边界条件测试 ====================

// 测试13：空队列size
TEST_F(TaskQueueTest, EmptyQueueSize) {
    EXPECT_EQ(queue_->size(), 0);
    EXPECT_TRUE(queue_->empty());
}

// 测试14：单个任务size
TEST_F(TaskQueueTest, SingleTaskSize) {
    queue_->push(Task([]() {}));
    EXPECT_EQ(queue_->size(), 1);
    EXPECT_FALSE(queue_->empty());
}

// 测试15：大量任务size
TEST_F(TaskQueueTest, ManyTasksSize) {
    const int count = 1000;
    for (int i = 0; i < count; ++i) {
        queue_->push(Task([i]() {}));
    }
    EXPECT_EQ(queue_->size(), count);
}

// 测试16：pop后size减少
TEST_F(TaskQueueTest, PopReducesSize) {
    for (int i = 0; i < 10; ++i) {
        queue_->push(Task([i]() {}));
    }
    
    EXPECT_EQ(queue_->size(), 10);
    
    for (int i = 0; i < 10; ++i) {
        Task task;
        queue_->pop(task);
        EXPECT_EQ(queue_->size(), 9 - i);
    }
    
    EXPECT_TRUE(queue_->empty());
}

// ==================== 任务有效性测试 ====================

// 测试17：空任务
TEST_F(TaskQueueTest, EmptyTask) {
    Task empty_task;
    EXPECT_FALSE(empty_task.is_valid());
    
    queue_->push(empty_task);
    
    Task popped;
    queue_->pop(popped);
    EXPECT_FALSE(popped.is_valid());
}

// 测试18：任务重置
TEST_F(TaskQueueTest, TaskReset) {
    Task task([]() {});
    EXPECT_TRUE(task.is_valid());
    
    task.reset();
    EXPECT_FALSE(task.is_valid());
    EXPECT_EQ(task.callback, nullptr);
    EXPECT_EQ(task.fiber, nullptr);
}

// 测试19：混合任务类型
TEST_F(TaskQueueTest, MixedTaskTypes) {
    auto fiber = std::make_shared<Fiber>([]() {});
    
    queue_->push(Task([]() {}));        // 回调任务
    queue_->push(Task(fiber));           // 协程任务
    queue_->push(Task([]() {}));        // 回调任务
    
    EXPECT_EQ(queue_->size(), 3);
    
    Task t1, t2, t3;
    queue_->pop(t1);
    queue_->pop(t2);
    queue_->pop(t3);
    
    EXPECT_NE(t1.callback, nullptr);
    EXPECT_EQ(t1.fiber, nullptr);
    
    EXPECT_EQ(t2.callback, nullptr);
    EXPECT_NE(t2.fiber, nullptr);
    
    EXPECT_NE(t3.callback, nullptr);
    EXPECT_EQ(t3.fiber, nullptr);
}

// ==================== 压力测试 ====================

// 测试20：快速push和pop
TEST_F(TaskQueueTest, RapidPushPop) {
    const int iterations = 10000;
    
    for (int i = 0; i < iterations; ++i) {
        queue_->push(Task([i]() {}));
        
        Task task;
        EXPECT_TRUE(queue_->try_pop(task));
        EXPECT_TRUE(task.is_valid());
    }
    
    EXPECT_TRUE(queue_->empty());
}

// 测试21：批量push后批量pop
TEST_F(TaskQueueTest, BatchPushThenBatchPop) {
    const int batch_size = 1000;
    
    // 批量push
    for (int i = 0; i < batch_size; ++i) {
        queue_->push(Task([i]() {}));
    }
    
    EXPECT_EQ(queue_->size(), batch_size);
    
    // 批量pop
    int popped_count = 0;
    while (!queue_->empty()) {
        Task task;
        if (queue_->try_pop(task)) {
            popped_count++;
        }
    }
    
    EXPECT_EQ(popped_count, batch_size);
}

// 测试22：并发try_pop
TEST_F(TaskQueueTest, ConcurrentTryPop) {
    const int task_count = 100;
    const int thread_num = 5;
    std::atomic<int> successful_pops{0};
    
    // 添加任务
    for (int i = 0; i < task_count; ++i) {
        queue_->push(Task([i]() {}));
    }
    
    // 并发try_pop
    std::vector<std::thread> threads;
    for (int t = 0; t < thread_num; ++t) {
        threads.emplace_back([this, &successful_pops]() {
            Task task;
            while (queue_->try_pop(task)) {
                if (task.is_valid()) {
                    successful_pops.fetch_add(1);
                }
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(successful_pops.load(), task_count);
    EXPECT_TRUE(queue_->empty());
}

// ==================== 协程任务特定测试 ====================

// 测试23：协程任务执行
TEST_F(TaskQueueTest, ExecuteFiberTask) {
    int value = 0;
    auto fiber = std::make_shared<Fiber>([&value]() {
        value = 42;
    });
    
    queue_->push(Task(fiber));
    
    Task task;
    queue_->pop(task);
    ASSERT_NE(task.fiber, nullptr);
    
    task.fiber->resume();
    EXPECT_EQ(value, 42);
}

// 测试24：多个协程任务
TEST_F(TaskQueueTest, MultipleFiberTasks) {
    std::vector<int> values(5, 0);
    
    for (int i = 0; i < 5; ++i) {
        auto fiber = std::make_shared<Fiber>([&values, i]() {
            values[i] = i * 10;
        });
        queue_->push(Task(fiber));
    }
    
    for (int i = 0; i < 5; ++i) {
        Task task;
        queue_->pop(task);
        task.fiber->resume();
    }
    
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(values[i], i * 10);
    }
}

// 测试25：协程任务yield
TEST_F(TaskQueueTest, FiberTaskWithYield) {
    int step = 0;
    auto fiber = std::make_shared<Fiber>([&step]() {
        step = 1;
        Fiber::yield();
        step = 2;
    });
    
    queue_->push(Task(fiber));
    
    Task task;
    queue_->pop(task);
    
    // 第一次resume
    task.fiber->resume();
    EXPECT_EQ(step, 1);
    EXPECT_EQ(task.fiber->state(), Fiber::State::kSuspended);
    
    // 第二次resume
    task.fiber->resume();
    EXPECT_EQ(step, 2);
    EXPECT_EQ(task.fiber->state(), Fiber::State::kTerminated);
}
