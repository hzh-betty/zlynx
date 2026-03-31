#ifndef ZCOROUTINE_SCHED_H_
#define ZCOROUTINE_SCHED_H_

#include <cstddef>
#include <cstdint>
#include <functional>

namespace zcoroutine {

/**
 * @brief 运行时管理器。
 * @details 负责调度器生命周期、任务投递和协程句柄恢复。
 */
class Runtime;

/**
 * @brief 调度器句柄类型。
 */
class Scheduler;

/**
 * @brief 协程任务函数类型。
 */
using Task = std::function<void()>;

/**
 * @brief 协程栈模型。
 */
enum class StackModel : uint8_t {
  kShared = 0,
  kIndependent = 1,
};

/**
 * @brief 表示无限等待的超时时间常量。
 */
constexpr uint32_t kInfiniteTimeoutMs = static_cast<uint32_t>(-1);

/**
 * @brief 初始化协程调度系统。
 * @param scheduler_count 调度器数量，0 表示按硬件并发自动决定。
 * @return 无返回值。
 */
void init(uint32_t scheduler_count = 0);

/**
 * @brief 设置每个调度器的共享栈数量。
 * @details 仅在运行时未启动时生效，建议在首次 init/go 之前调用。
 * @param stack_num 共享栈数量，需大于 0。
 * @return 无返回值。
 */
void co_stack_num(size_t stack_num);

/**
 * @brief 设置协程栈大小（字节）。
 * @details 仅在运行时未启动时生效，建议在首次 init/go 之前调用。
 * @param stack_size 栈大小，需大于 0。
 * @return 无返回值。
 */
void co_stack_size(size_t stack_size);

/**
 * @brief 设置协程栈模型。
 * @details 仅在运行时未启动时生效，建议在首次 init/go 之前调用。
 * @param stack_model 栈模型，支持共享栈与独立栈。
 * @return 无返回值。
 */
void co_stack_model(StackModel stack_model);

/**
 * @brief 关闭协程调度系统并释放资源。
 * @param 无参数。
 * @return 无返回值。
 */
void shutdown();

/**
 * @brief 提交任务到运行时。
 * @param task 需要异步执行的任务。
 * @return 无返回值。
 */
void go(Task task);

/**
 * @brief 调度器轻量句柄。
 * @details 该句柄不拥有调度器，仅用于将任务投递到指定调度器。
 */
class Scheduler {
 public:
  /**
  * @brief 向当前句柄对应的调度器提交任务。
  * @param task 需要执行的任务。
  * @return 无返回值。
  */
  void go(Task task);

  /**
  * @brief 获取调度器编号。
  * @param 无参数。
  * @return 调度器编号。
  */
  int id() const;

 private:
  friend class Runtime;
  friend Scheduler* main_sched();
  friend Scheduler* next_sched();
  explicit Scheduler(size_t scheduler_index);

  size_t scheduler_index_;
};

/**
 * @brief 获取主调度器句柄。
 * @param 无参数。
 * @return 主调度器句柄，若运行时不可用则返回 nullptr。
 */
Scheduler* main_sched();

/**
 * @brief 轮询获取下一个调度器句柄。
 * @param 无参数。
 * @return 调度器句柄，若运行时不可用则返回 nullptr。
 */
Scheduler* next_sched();

/**
 * @brief 停止所有调度器。
 * @param 无参数。
 * @return 无返回值。
 */
void stop_scheds();

/**
 * @brief 当前协程主动让出执行权。
 * @param 无参数。
 * @return 无返回值。
 */
void yield();

/**
 * @brief 当前执行体休眠指定毫秒。
 * @param milliseconds 休眠时长，单位毫秒。
 * @return 无返回值。
 */
void sleep_for(uint32_t milliseconds);

/**
 * @brief 恢复一个已挂起协程。
 * @param fiber 协程句柄地址。
 * @return 无返回值。
 */
void resume(void* fiber);

/**
 * @brief 获取当前协程句柄。
 * @param 无参数。
 * @return 当前协程句柄，不在协程上下文则返回 nullptr。
 */
void* current_coroutine();

/**
 * @brief 获取当前调度器编号。
 * @param 无参数。
 * @return 调度器编号，不在调度线程中返回 -1。
 */
int sched_id();

/**
 * @brief 获取当前协程编号。
 * @param 无参数。
 * @return 协程编号，不在协程上下文中返回 -1。
 */
int coroutine_id();

/**
 * @brief 判断最近一次阻塞是否超时。
 * @param 无参数。
 * @return true 表示最近一次阻塞因超时返回。
 */
bool timeout();

/**
 * @brief 判断当前是否处于协程上下文。
 * @param 无参数。
 * @return true 表示当前在协程中执行。
 */
bool in_coroutine();

/**
 * @brief 获取当前运行时调度器总数。
 * @param 无参数。
 * @return 调度器数量。
 */
size_t scheduler_count();

}  // namespace zcoroutine

#endif  // ZCOROUTINE_SCHED_H_
