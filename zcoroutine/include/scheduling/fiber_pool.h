#ifndef ZCOROUTINE_FIBER_POOL_H_
#define ZCOROUTINE_FIBER_POOL_H_

#include <deque>
#include <mutex>
#include <atomic>
#include <memory>
#include "runtime/fiber.h"

namespace zcoroutine {

/**
 * @brief 协程池统计信息
 */
struct PoolStatistics {
    size_t total_created = 0;   // 累计创建的协程数
    size_t total_reused = 0;    // 累计复用的协程数
    size_t idle_count = 0;      // 当前空闲协程数
};

/**
 * @brief 协程池类
 * 管理协程对象池，支持协程复用，减少创建销毁开销
 */
class FiberPool {
public:
    using ptr = std::shared_ptr<FiberPool>;

    /**
     * @brief 构造函数
     * @param min_size 最小容量
     * @param max_size 最大容量
     */
    FiberPool(size_t min_size = 10, size_t max_size = 1000);

    /**
     * @brief 析构函数
     */
    ~FiberPool();

    /**
     * @brief 获取一个可用协程
     * @param func 协程执行函数
     * @return 协程智能指针
     * 如果池中有空闲协程则复用，否则创建新协程
     */
    Fiber::ptr get(std::function<void()> func);

    /**
     * @brief 归还协程到池中
     * @param fiber 协程指针
     * 协程必须处于Terminated状态才能归还
     */
    void release(Fiber::ptr fiber);

    /**
     * @brief 调整池大小
     * @param size 新的最大容量
     */
    void resize(size_t size);

    /**
     * @brief 清空池
     * 释放所有空闲协程
     */
    void clear();

    /**
     * @brief 获取空闲协程数量
     * @return 空闲协程数
     */
    size_t get_idle_count() const;

    /**
     * @brief 获取统计信息
     * @return 统计信息结构
     */
    PoolStatistics get_statistics() const;

private:
    size_t min_size_;                       // 最小容量
    size_t max_size_;                       // 最大容量
    std::deque<Fiber::ptr> idle_fibers_;    // 空闲协程队列
    mutable std::mutex mutex_;              // 保护池的互斥锁
    std::atomic<size_t> total_created_;     // 累计创建数
    std::atomic<size_t> total_reused_;      // 累计复用数
};

} // namespace zcoroutine

#endif // ZCOROUTINE_FIBER_POOL_H_
