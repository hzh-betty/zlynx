#ifndef ZCO_INTERNAL_RUNTIME_MANAGER_H_
#define ZCO_INTERNAL_RUNTIME_MANAGER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "zco/internal/fiber.h"
#include "zco/internal/fiber_handle_registry.h"
#include "zco/internal/noncopyable.h"
#include "zco/internal/processor.h"
#include "zco/internal/timer.h"
#include "zco/sched.h"

namespace zco {

class Scheduler;

/**
 * @brief 协程运行时管理器。
 * @details 管理 Processor 集合、任务分发和 Fiber 句柄生命周期。
 */
class Runtime : public NonCopyable {
  public:
    /**
     * @brief 获取运行时单例。
     * @param 无参数。
     * @return 运行时引用。
     */
    static Runtime &instance();

    /**
     * @brief 初始化运行时。
     * @param scheduler_count 调度器数量，0 表示自动选择。
     * @return 无返回值。
     */
    void init(uint32_t scheduler_count);

    /**
     * @brief 设置每个调度器共享栈数量。
     * @details 仅在运行时未启动时生效。
     * @param stack_num 共享栈数量。
     * @return true 表示设置成功。
     */
    bool set_stack_num(size_t stack_num);

    /**
     * @brief 设置协程栈大小。
     * @details 仅在运行时未启动时生效。
     * @param stack_size 栈大小（字节）。
     * @return true 表示设置成功。
     */
    bool set_stack_size(size_t stack_size);

    /**
     * @brief 设置协程栈模型。
     * @details 仅在运行时未启动时生效。
     * @param stack_model 栈模型。
     * @return true 表示设置成功。
     */
    bool set_stack_model(StackModel stack_model);

    /**
     * @brief 获取当前配置的共享栈数量。
     * @param 无参数。
     * @return 共享栈数量。
     */
    size_t stack_num() const;

    /**
     * @brief 获取当前配置的协程栈大小。
     * @param 无参数。
     * @return 栈大小。
     */
    size_t stack_size() const;

    /**
     * @brief 获取当前配置的栈模型。
     * @param 无参数。
     * @return 栈模型。
     */
    StackModel stack_model() const;

    /**
     * @brief 关闭运行时。
     * @param 无参数。
     * @return 无返回值。
     */
    void shutdown();

    /**
     * @brief 投递任务。
     * @param task 任务函数。
     * @return 无返回值。
     */
    void submit(Task task);

    /**
     * @brief 向指定调度器投递任务。
     * @param scheduler_index 调度器索引。
     * @param task 任务函数。
     * @return 无返回值。
     */
    void submit_to(size_t scheduler_index, Task task);

    /**
     * @brief 获取主调度器句柄。
     * @param 无参数。
     * @return 调度器句柄。
     */
    Scheduler *main_scheduler();

    /**
     * @brief 获取下一个调度器句柄。
     * @param 无参数。
     * @return 调度器句柄。
     */
    Scheduler *next_scheduler();

    /**
     * @brief 通过外部句柄恢复 Fiber。
     * @param handle 协程外部句柄。
     * @return 无返回值。
     */
    void resume_external(void *handle);

    /**
     * @brief 获取调度器数量。
     * @param 无参数。
     * @return 调度器数量。
     */
    size_t scheduler_count() const;

    /**
     * @brief 获取处理器列表。
     * @param 无参数。
     * @return 处理器数组引用。
     */
    const std::vector<std::unique_ptr<Processor>> &processors() const;

    /**
     * @brief 生成新 Fiber 编号。
     * @param 无参数。
     * @return Fiber 编号。
     */
    int next_fiber_id();

    /**
     * @brief 注册 Fiber 句柄。
     * @param fiber 协程对象。
     * @return 无返回值。
     */
    void register_fiber(const Fiber::ptr &fiber);

    /**
     * @brief 注销 Fiber 句柄。
     * @param fiber 协程裸指针。
     * @return 无返回值。
     */
    void unregister_fiber(Fiber *fiber);

    /**
     * @brief 导出 Fiber 的外部句柄。
     * @param fiber 协程对象。
     * @return 对外可传递句柄，不存在时返回 nullptr。
     */
    void *external_handle(const Fiber::ptr &fiber);

    /**
     * @brief 确保调度器句柄存在。
     * @param scheduler_index 调度器索引。
     * @return 调度器句柄。
     */
    Scheduler *ensure_scheduler_handle(size_t scheduler_index);

    /**
     * @brief 确保运行时已启动。
     * @param 无参数。
     * @return true 表示可用。
     */
    bool ensure_started();

    /**
     * @brief 选择任务投递目标处理器索引。
     * @param 无参数。
     * @return 处理器索引。
     */
    size_t pick_processor_index();

    /**
     * @brief 计算二选一候选索引。
     * @param first 首个候选。
     * @param ticket 序号。
     * @return 第二个候选索引。
     */
    size_t pick_secondary_index(size_t first, uint64_t ticket);

  private:
    /**
     * @brief 构造运行时对象。
     * @param 无参数。
     * @return 无返回值。
     */
    Runtime();

    std::atomic<bool> started_;
    std::atomic<uint32_t> rr_index_; // 轮询索引，用于简单的轮询负载均衡
    std::atomic<uint64_t>
        chooser_seed_; // 选择器种子，用于生成随机数实现负载均衡
    std::atomic<int> fiber_id_gen_; // Fiber 编号生成器，递增分配唯一编号
    std::atomic<uint64_t>
        fiber_handle_id_gen_; // Fiber 句柄 id 生成器，递增分配唯一 id
    std::vector<std::unique_ptr<Processor>> processors_;

    mutable std::mutex stack_config_mutex_;
    size_t stack_num_;
    size_t stack_size_;
    StackModel stack_model_;

    mutable std::mutex scheduler_handle_mutex_;
    std::vector<std::unique_ptr<Scheduler>> scheduler_handles_;

    FiberHandleRegistry fiber_handle_registry_;
};

/**
 * @brief 获取当前 Fiber。
 * @param 无参数。
 * @return 当前 Fiber，共享指针；不在处理器线程返回 nullptr。
 */
Fiber::ptr current_fiber_shared();

/**
 * @brief 恢复等待中的 Fiber。
 * @param fiber 协程对象。
 * @param timed_out 是否因超时恢复。
 * @return 无返回值。
 */
void resume_fiber(const Fiber::ptr &fiber, bool timed_out);

/**
 * @brief 将当前 Fiber 标记为等待。
 * @param 无参数。
 * @return 无返回值。
 */
void prepare_current_wait();

/**
 * @brief 挂起当前 Fiber。
 * @param 无参数。
 * @return true 表示非超时恢复。
 */
bool park_current();

/**
 * @brief 带超时挂起当前 Fiber。
 * @param milliseconds 超时毫秒。
 * @return true 表示非超时恢复。
 */
bool park_current_for(uint32_t milliseconds);

/**
 * @brief 在当前处理器添加定时器。
 * @param milliseconds 延时毫秒。
 * @param callback 回调函数。
 * @return 定时器令牌。
 */
std::shared_ptr<TimerToken> add_timer(uint32_t milliseconds,
                                      std::function<void()> callback);

/**
 * @brief 等待 fd 事件。
 * @details 仅允许在协程上下文调用。
 * @param fd 文件描述符。
 * @param events 事件掩码。
 * @param milliseconds 超时毫秒。
 * @return true 表示事件已就绪。
 */
bool wait_fd(int fd, uint32_t events, uint32_t milliseconds);

} // namespace zco

#endif // ZCO_INTERNAL_RUNTIME_MANAGER_H_
