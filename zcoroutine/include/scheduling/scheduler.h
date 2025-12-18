#ifndef ZCOROUTINE_SCHEDULER_H_
#define ZCOROUTINE_SCHEDULER_H_

#include <string>
#include <vector>
#include <thread>
#include <memory>
#include <atomic>
#include <functional>
#include "scheduling/task_queue.h"
#include "scheduling/fiber_pool.h"
#include "runtime/fiber.h"

namespace zcoroutine {

/**
 * @brief 调度器类
 * 基于线程池的M:N调度模型
 * 使用std::thread和std::mutex，不再封装Thread/Mutex类
 */
class Scheduler {
public:
    using ptr = std::shared_ptr<Scheduler>;

    /**
     * @brief 构造函数
     * @param thread_count 线程数量
     * @param name 调度器名称
     */
    explicit Scheduler(int thread_count = 1, const std::string& name = "Scheduler");

    /**
     * @brief 析构函数
     */
    virtual ~Scheduler();

    /**
     * @brief 获取调度器名称
     */
    const std::string& name() const { return name_; }

    /**
     * @brief 启动调度器
     */
    void start();

    /**
     * @brief 停止调度器
     * 等待所有任务执行完毕后停止
     */
    void stop();

    /**
     * @brief 调度协程
     * @param fiber 协程指针
     */
    void schedule(Fiber::ptr fiber);

    /**
     * @brief 调度函数
     * @param func 回调函数
     */
    void schedule(std::function<void()> func);

    /**
     * @brief 模板方法：调度可调用对象
     * @tparam F 函数类型
     * @tparam Args 参数类型
     */
    template<class F, class... Args>
    void schedule(F&& f, Args&&... args) {
        schedule(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
    }

    /**
     * @brief 设置协程池
     * @param pool 协程池指针
     */
    void set_fiber_pool(FiberPool::ptr pool);

    /**
     * @brief 获取协程池
     */
    FiberPool::ptr get_fiber_pool() const { return fiber_pool_; }

    /**
     * @brief 是否正在运行
     */
    bool is_running() const { return !stopping_.load(std::memory_order_relaxed); }

    /**
     * @brief 获取当前调度器（线程本地）
     */
    static Scheduler* get_this();

    /**
     * @brief 设置当前调度器（线程本地）
     */
    static void set_this(Scheduler* scheduler);

protected:
    /**
     * @brief 工作线程主循环
     */
    void run();

private:
    std::string name_;                              // 调度器名称
    int thread_count_;                              // 线程数量
    std::vector<std::unique_ptr<std::thread>> threads_;  // 线程池（使用std::thread）
    std::unique_ptr<TaskQueue> task_queue_;         // 任务队列
    FiberPool::ptr fiber_pool_;                     // 协程池（可选）
    
    std::atomic<bool> stopping_;                    // 停止标志
    std::atomic<int> active_thread_count_;          // 活跃线程数
    std::atomic<int> idle_thread_count_;            // 空闲线程数
};

} // namespace zcoroutine

#endif // ZCOROUTINE_SCHEDULER_H_
