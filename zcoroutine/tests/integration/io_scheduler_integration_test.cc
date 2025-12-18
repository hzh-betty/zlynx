/**
 * @file io_scheduler_integration_test.cc
 * @brief IoScheduler集成测试
 * 测试调度器、协程、IO事件、定时器的协同工作
 */

#include <gtest/gtest.h>
#include "io/io_scheduler.h"
#include "hook/hook.h"
#include "runtime/fiber.h"
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <thread>
#include <chrono>

using namespace zcoroutine;

class IoSchedulerIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        set_hook_enable(true);
    }

    void TearDown() override {
        set_hook_enable(false);
    }
};

// 测试：IoScheduler基本功能
TEST_F(IoSchedulerIntegrationTest, BasicIoScheduler) {
    auto io_scheduler = IoScheduler::CreateInstance(2, true, "TestIoScheduler");
    EXPECT_NE(io_scheduler, nullptr);
    
    std::atomic<int> count{0};
    
    io_scheduler->schedule([&count]() {
        count++;
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    io_scheduler->stop();
    
    EXPECT_GE(count.load(), 1);
}

// 测试：定时器功能
TEST_F(IoSchedulerIntegrationTest, TimerFunction) {
    auto io_scheduler = IoScheduler::CreateInstance(2, true, "TimerTest");
    
    std::atomic<int> timer_count{0};
    
    // 添加单次定时器
    io_scheduler->add_timer(100, [&timer_count]() {
        timer_count++;
    });
    
    // 添加循环定时器
    std::atomic<int> loop_count{0};
    auto timer = io_scheduler->add_timer(50, [&loop_count]() {
        loop_count++;
    }, true);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    EXPECT_GE(timer_count.load(), 1);
    EXPECT_GE(loop_count.load(), 5); // 500ms / 50ms ≈ 10次
    
    timer->cancel();
    io_scheduler->stop();
}

// 测试：Pipe IO事件
TEST_F(IoSchedulerIntegrationTest, PipeIoEvent) {
    auto io_scheduler = IoScheduler::CreateInstance(2, true, "PipeTest");
    
    int pipe_fds[2];
    ASSERT_EQ(pipe(pipe_fds), 0);
    
    // 设置非阻塞
    fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
    fcntl(pipe_fds[1], F_SETFL, O_NONBLOCK);
    
    std::atomic<bool> read_done{false};
    std::string received_data;
    
    // 添加读事件
    io_scheduler->add_event(pipe_fds[0], FdContext::kRead, [&]() {
        char buffer[256];
        ssize_t n = read(pipe_fds[0], buffer, sizeof(buffer) - 1);
        if (n > 0) {
            buffer[n] = '\0';
            received_data = buffer;
            read_done = true;
        }
    });
    
    // 延迟写入数据
    io_scheduler->add_timer(100, [&]() {
        const char* msg = "Hello IoScheduler!";
        write(pipe_fds[1], msg, strlen(msg));
    });
    
    // 等待读取完成
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    
    EXPECT_TRUE(read_done.load());
    EXPECT_EQ(received_data, "Hello IoScheduler!");
    
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    io_scheduler->stop();
}

// 测试：协程与IO事件结合
TEST_F(IoSchedulerIntegrationTest, FiberWithIoEvent) {
    auto io_scheduler = IoScheduler::CreateInstance(2, true, "FiberIoTest");
    
    int pipe_fds[2];
    ASSERT_EQ(pipe(pipe_fds), 0);
    fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
    fcntl(pipe_fds[1], F_SETFL, O_NONBLOCK);
    
    std::atomic<int> step{0};
    
    // 写协程
    io_scheduler->schedule([&]() {
        step = 1;
        const char* msg = "fiber message";
        write(pipe_fds[1], msg, strlen(msg));
        step = 2;
    });
    
    // 读协程
    io_scheduler->schedule([&]() {
        io_scheduler->add_event(pipe_fds[0], FdContext::kRead, [&]() {
            char buffer[256];
            ssize_t n = read(pipe_fds[0], buffer, sizeof(buffer) - 1);
            if (n > 0) {
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
    auto io_scheduler = IoScheduler::CreateInstance(4, true, "MultiIoTest");
    
    const int fiber_count = 10;
    std::atomic<int> completed{0};
    
    for (int i = 0; i < fiber_count; ++i) {
        io_scheduler->schedule([&, i]() {
            // 模拟IO操作
            io_scheduler->add_timer(50 + i * 10, [&]() {
                completed++;
            });
        });
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    EXPECT_EQ(completed.load(), fiber_count);
    io_scheduler->stop();
}

// 测试：Hook系统调用
TEST_F(IoSchedulerIntegrationTest, HookSystemCall) {
    auto io_scheduler = IoScheduler::CreateInstance(2, true, "HookTest");
    
    std::atomic<bool> sleep_done{false};
    
    io_scheduler->schedule([&]() {
        auto start = std::chrono::steady_clock::now();
        sleep(1); // 应该被hook，转换为异步定时器
        auto end = std::chrono::steady_clock::now();
        
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        EXPECT_GE(duration, 1000);
        sleep_done = true;
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    EXPECT_TRUE(sleep_done.load());
    
    io_scheduler->stop();
}
