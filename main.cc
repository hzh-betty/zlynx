#include "zlynx_logger.h"

#include "fiber.h"
#include "scheduler.h"

void test1()
{
    try
    {
        ZLYNX_LOG_INFO("main start");
        auto f = std::make_shared<zlynx::Fiber>([]
        {
            ZLYNX_LOG_INFO("fiber start");
            zlynx::Fiber::get_fiber()->yield();
            ZLYNX_LOG_INFO("fiber resume");
        });
        zlynx::Fiber::get_fiber();
        f->resume();
        ZLYNX_LOG_INFO("after first resume");
        f->resume();
        ZLYNX_LOG_INFO("main end");
    }
    catch (const std::exception &e)
    {
        ZLYNX_LOG_FATAL("Unhandled exception: {}", e.what());
    }
}

void test2()
{
    try
    {
        ZLYNX_LOG_INFO("Scheduler test start");

        // 创建调度器，使用2个线程
        zlynx::Scheduler scheduler(2, true, "TestScheduler");

        // 启动调度器
        scheduler.start();

        // 提交任务
        for (int i = 0; i < 5; ++i)
        {
            scheduler.schedule([i]()
            {
                ZLYNX_LOG_INFO("Task {} running in thread {}", i, zlynx::Thread::get_name());
                zlynx::Fiber::get_fiber()->yield(); // 模拟任务挂起
                ZLYNX_LOG_INFO("Task {} resumed in thread {}", i, zlynx::Thread::get_name());
            });
        }

        // 停止调度器
        scheduler.stop();
    }
    catch (const std::exception &e)
    {
        ZLYNX_LOG_ERROR("Scheduler test exception: {}", e.what());
    }


    ZLYNX_LOG_INFO("Scheduler test end");
}


int main()
{
    zlynx::Init(); // 初始化日志系统
    // test1();
    test2();
    return 0;
}
