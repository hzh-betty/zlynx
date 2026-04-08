#ifndef ZCOROUTINE_INTERNAL_FIBER_H_
#define ZCOROUTINE_INTERNAL_FIBER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "zcoroutine/internal/context.h"
#include "zcoroutine/internal/noncopyable.h"
#include "zcoroutine/sched.h"

namespace zcoroutine {

class Processor;

/**
 * @brief 协程执行单元。
 * @details 负责维护协程上下文、状态机与共享栈快照。
 */
class Fiber : public std::enable_shared_from_this<Fiber>, public NonCopyable {
 public:
 using ptr = std::shared_ptr<Fiber>;

  /**
  * @brief 协程状态。
  */
  enum class State : uint8_t {
    kReady = 0,
    kRunning = 1,
    kWaiting = 2,
    kDone = 3,
  };

  /**
   * @brief 构造协程对象。
   * @param id 协程编号。
   * @param owner 所属处理器。
   * @param task 协程任务函数。
   * @param stack_size 共享栈大小。
   * @return 无返回值。
   */
    Fiber(int id,
      Processor* owner,
      Task task,
      size_t stack_size,
      size_t stack_slot,
      bool use_shared_stack);

  /**
   * @brief 析构协程对象。
   * @param 无参数。
   * @return 无返回值。
   */
  ~Fiber();

  /**
   * @brief 获取协程编号。
   * @param 无参数。
   * @return 协程编号。
   */
  int id() const;

  /**
   * @brief 重置协程对象用于复用。
   * @param id 新协程编号。
   * @param task 新任务。
   * @param stack_slot 共享栈槽位。
   * @return 无返回值。
   */
  void reset(int id, Task task, size_t stack_slot);

  /**
   * @brief 获取所属处理器。
   * @param 无参数。
   * @return 处理器裸指针（非拥有关系）。
   */
  Processor* owner() const;

  /**
   * @brief 获取绑定的共享栈槽位。
   * @param 无参数。
   * @return 槽位编号。
   */
  size_t stack_slot() const;

  /**
   * @brief 判断当前协程是否使用共享栈模式。
   * @param 无参数。
   * @return true 表示共享栈模式。
   */
  bool use_shared_stack() const;

  /**
   * @brief 获取外部句柄 id。
   * @param 无参数。
   * @return 句柄 id；0 表示未注册。
   */
  uint64_t external_handle_id() const;

  /**
   * @brief 尝试设置外部句柄 id（仅允许从 0 初始化一次）。
   * @param handle_id 待设置句柄。
   * @param effective_handle 输出最终有效句柄。
   * @return true 表示得到有效句柄（新设或已存在）。
   */
  bool try_set_external_handle_id(uint64_t handle_id, uint64_t* effective_handle);

  /**
   * @brief 清空外部句柄 id。
   * @param 无参数。
   * @return 清空前的句柄 id。
   */
  uint64_t clear_external_handle_id();

  /**
   * @brief 获取上下文对象。
   * @param 无参数。
    * @return Context 指针。
   */
    Context* context();

  /**
   * @brief 获取协程状态。
   * @param 无参数。
   * @return 当前状态值。
   */
  State state() const;

  /**
   * @brief 标记协程为运行态。
   * @param 无参数。
   * @return 无返回值。
   */
  void mark_running();

  /**
   * @brief 标记协程为就绪态。
   * @param 无参数。
   * @return 无返回值。
   */
  void mark_ready();

  /**
   * @brief 标记协程为等待态。
   * @param 无参数。
   * @return 无返回值。
   */
  void mark_waiting();

  /**
   * @brief 标记协程为完成态。
   * @param 无参数。
   * @return 无返回值。
   */
  void mark_done();

  /**
   * @brief 尝试从等待态唤醒。
   * @param timed_out 是否超时唤醒。
   * @return true 表示状态转换成功。
   */
  bool try_wake(bool timed_out);

  /**
   * @brief 查询最近一次阻塞是否超时。
   * @param 无参数。
   * @return true 表示超时。
   */
  bool timed_out() const;

  /**
   * @brief 清理超时标记。
   * @param 无参数。
   * @return 无返回值。
   */
  void clear_timed_out();

  /**
   * @brief 执行协程入口函数。
   * @param 无参数。
   * @return 无返回值。
   */
  void run();

  /**
   * @brief 判断上下文是否已初始化。
   * @param 无参数。
   * @return true 表示已初始化。
   */
  bool context_initialized() const;

  /**
   * @brief 初始化协程上下文。
   * @param 无参数。
   * @return 无返回值。
   */
  void initialize_context();

  /**
   * @brief 保存共享栈快照数据。
   * @param data 栈快照起始地址。
   * @param size 栈快照大小。
   * @return 无返回值。
   */
  void save_stack_data(const char* data, size_t size);

  /**
   * @brief 判断是否存在已保存栈快照。
   * @param 无参数。
   * @return true 表示存在快照。
   */
  bool has_saved_stack() const;

  /**
   * @brief 获取已保存快照大小。
   * @param 无参数。
   * @return 快照字节数。
   */
  size_t saved_stack_size() const;

  /**
   * @brief 获取已保存快照数据指针。
   * @param 无参数。
   * @return 快照数据地址；无快照时返回 nullptr。
   */
  const char* saved_stack_data() const;

  /**
   * @brief 清除已保存快照。
   * @param 无参数。
   * @return 无返回值。
   */
  void clear_saved_stack();

 private:
  int id_;
  Processor* owner_;
  Task task_;
  size_t stack_slot_;
  bool use_shared_stack_;
  char* independent_stack_buffer_;
  size_t independent_stack_size_;
  Context context_;
  char* saved_stack_buffer_;
  size_t saved_stack_size_;
  size_t saved_stack_capacity_;
  uint8_t saved_stack_bucket_;
  bool context_initialized_;
  std::atomic<State> state_;
  std::atomic<bool> timed_out_;
  std::atomic<uint64_t> external_handle_id_;
};

}  // namespace zcoroutine

#endif  // ZCOROUTINE_INTERNAL_FIBER_H_
