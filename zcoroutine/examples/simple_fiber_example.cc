/**
 * @file simple_fiber_example.cc
 * @brief zcoroutine基本使用示例
 * 
 * 演示：
 * 1. 创建协程
 * 2. 使用调度器调度协程
 * 3. 协程切换和状态管理
 */

#include <iostream>
#include "zcoroutine_logger.h"
#include "runtime/fiber.h"
#include "scheduling/scheduler.h"

using namespace zcoroutine;

// 简单的协程函数
void simple_fiber_func() {
    auto fiber = Fiber::get_this();
    std::cout << "协程 [" << fiber->name() << ":" << fiber->id() << "] 开始执行" << std::endl;
    
    // 模拟一些工作
    for (int i = 0; i < 3; ++i) {
        std::cout << "协程 [" << fiber->name() << ":" << fiber->id() << "] 执行步骤 " << i << std::endl;
        
        // 主动让出CPU
        if (i < 2) {
            Fiber::yield();
        }
    }
    
    std::cout << "协程 [" << fiber->name() << ":" << fiber->id() << "] 执行完成" << std::endl;
}

// 带参数的协程函数
void fiber_with_param(int value, const std::string& message) {
    auto fiber = Fiber::get_this();
    std::cout << "协程 [" << fiber->name() << ":" << fiber->id() << "] "
              << "参数: value=" << value << ", message=" << message << std::endl;
}

int main() {
    // 初始化日志系统
    InitLogger(zlog::LogLevel::value::DEBUG);
    
    std::cout << "=== zcoroutine基本示例 ===" << std::endl;
    
    // 示例1: 创建单个协程并执行
    {
        std::cout << "\n[示例1] 创建单个协程" << std::endl;
        
        auto fiber = std::make_shared<Fiber>(simple_fiber_func, 
                                             StackAllocator::kDefaultStackSize, 
                                             "worker");
        
        std::cout << "协程已创建: " << fiber->name() << ", 状态: " 
                  << static_cast<int>(fiber->state()) << std::endl;
        
        // 恢复协程执行
        fiber->resume();
        
        std::cout << "第一次resume后, 状态: " << static_cast<int>(fiber->state()) << std::endl;
        
        // 再次恢复
        fiber->resume();
        fiber->resume();
        
        std::cout << "协程执行完毕, 状态: " << static_cast<int>(fiber->state()) << std::endl;
    }
    
    // 示例2: 使用调度器调度多个协程
    {
        std::cout << "\n[示例2] 使用调度器调度多个协程" << std::endl;
        
        // 创建调度器（4个工作线程）
        auto scheduler = std::make_shared<Scheduler>(4, "MainScheduler");
        scheduler->start();
        
        // 调度多个协程
        for (int i = 0; i < 10; ++i) {
            auto fiber = std::make_shared<Fiber>(
                [i]() {
                    auto f = Fiber::get_this();
                    std::cout << "协程 [" << f->name() << "] 任务编号: " << i << std::endl;
                },
                StackAllocator::kDefaultStackSize,
                "task_" + std::to_string(i)
            );
            
            scheduler->schedule(fiber);
        }
        
        // 也可以直接调度函数
        scheduler->schedule([]() {
            std::cout << "这是一个直接调度的函数" << std::endl;
        });
        
        // 等待一段时间让任务执行完成
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        // 停止调度器
        scheduler->stop();
        
        std::cout << "调度器已停止" << std::endl;
    }
    
    // 示例3: 使用协程池
    {
        std::cout << "\n[示例3] 使用协程池" << std::endl;
        
        // 创建协程池
        auto fiber_pool = std::make_shared<FiberPool>(5, 100);
        
        // 创建调度器并设置协程池
        auto scheduler = std::make_shared<Scheduler>(2, "PoolScheduler");
        scheduler->set_fiber_pool(fiber_pool);
        scheduler->start();
        
        // 调度任务
        for (int i = 0; i < 20; ++i) {
            auto fiber = std::make_shared<Fiber>(
                [i]() {
                    auto f = Fiber::get_this();
                    std::cout << "池协程 [" << f->name() << "] 执行任务 " << i << std::endl;
                },
                StackAllocator::kDefaultStackSize,
                "pool_fiber"
            );
            
            scheduler->schedule(fiber);
        }
        
        // 等待执行完成
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        // 查看协程池统计信息
        auto stats = fiber_pool->get_statistics();
        std::cout << "协程池统计:" << std::endl;
        std::cout << "  创建总数: " << stats.total_created << std::endl;
        std::cout << "  复用总数: " << stats.total_reused << std::endl;
        std::cout << "  空闲数量: " << stats.idle_count << std::endl;
        
        scheduler->stop();
    }
    
    std::cout << "\n=== 示例执行完成 ===" << std::endl;
    
    return 0;
}
