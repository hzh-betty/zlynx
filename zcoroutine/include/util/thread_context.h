#ifndef ZCOROUTINE_THREAD_CONTEXT_H_
#define ZCOROUTINE_THREAD_CONTEXT_H_

namespace zcoroutine {

// 前向声明
class Fiber;
class Scheduler;

/**
 * @brief 线程上下文类
 * 集中管理线程本地状态，包括当前协程、调度器协程、调度器指针等
 * 使用thread_local变量存储，每个线程独立
 */
class ThreadContext {
public:
    /**
     * @brief 获取当前线程的上下文
     * @return 当前线程的ThreadContext指针，如果不存在则创建
     */
    static ThreadContext* GetCurrent();

    /**
     * @brief 设置当前执行的协程
     * @param fiber 协程指针
     */
    static void SetCurrentFiber(Fiber* fiber);

    /**
     * @brief 获取当前执行的协程
     * @return 当前协程指针
     */
    static Fiber* GetCurrentFiber();

    /**
     * @brief 设置调度器协程
     * @param fiber 调度器协程指针
     */
    static void SetSchedulerFiber(Fiber* fiber);

    /**
     * @brief 获取调度器协程
     * @return 调度器协程指针
     */
    static Fiber* GetSchedulerFiber();

    /**
     * @brief 设置当前调度器
     * @param scheduler 调度器指针
     */
    static void SetScheduler(Scheduler* scheduler);

    /**
     * @brief 获取当前调度器
     * @return 调度器指针
     */
    static Scheduler* GetScheduler();

private:
    Fiber* current_fiber_ = nullptr;      // 当前执行的协程
    Fiber* scheduler_fiber_ = nullptr;    // 调度器协程
    Scheduler* scheduler_ = nullptr;      // 当前调度器
};

} // namespace zcoroutine

#endif // ZCOROUTINE_THREAD_CONTEXT_H_
