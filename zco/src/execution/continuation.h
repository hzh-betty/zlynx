#ifndef ZLYNX_ZCO_SRC_EXECUTION_CONTINUATION_H_
#define ZLYNX_ZCO_SRC_EXECUTION_CONTINUATION_H_

#include "execution/linux/native_context.h"
#include "execution/stack_arena.h"

namespace zco {
namespace detail {
#ifdef ZCO_BENCHMARK_METRICS
uint64_t snapshot_bytes_copied();
#endif

/**
 * @brief 一个任务的执行上下文及其栈存储关系，只在所属工作线程切换。
 *
 * 独立模式拥有栈；共享模式借用 Arena 槽位，挂起后保存快照、恢复前还原。
 * Arena 须覆盖本对象的生命周期，快照操作只能在调用方栈上进行。
 */
class Continuation {
  public:
    Continuation(StackArena &arena, void (*entry)());
    ~Continuation();
    void resume(NativeContext &caller);
    void suspend(NativeContext &caller);

    /**
     * @brief 从原栈位置恢复挂起任务，使 suspend() 抛出指定异常以展开栈。
     * @pre failure
     * 为非空异常，挂起栈仍在原槽位且未被覆盖；用于快照保存失败的收尾。
     */
    void interrupt(NativeContext &caller, std::exception_ptr failure);

    /**
     * @brief 保存共享栈从当前栈指针到栈顶的活动区；独立栈无需复制。
     * @pre 任务已挂起，当前已切回调用方栈，原槽位尚未被复用。
     */
    void save();

  private:
    StackArena &arena_;
    std::unique_ptr<StackBuffer> independent_;
    StackBuffer *stack_;
    NativeContext context_;
    std::vector<char> snapshot_; // 快照数据
    std::exception_ptr interruption_;
};
} // namespace detail
} // namespace zco

#endif
