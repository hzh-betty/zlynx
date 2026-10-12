#ifndef ZLYNX_ZCO_SRC_EXECUTION_LINUX_NATIVE_CONTEXT_H_
#define ZLYNX_ZCO_SRC_EXECUTION_LINUX_NATIVE_CONTEXT_H_

#include <cstddef>
#include <ucontext.h>

namespace zco {
namespace detail {
/**
 * @brief Linux 协程寄存器上下文及任务私有的 errno 快照。
 *
 * 同一工作线程上的协程共享线程局部 errno，切换时须与上下文一并保存和恢复，
 * 避免其他协程的系统调用覆盖当前任务的错误状态。
 */
struct NativeContext {
    ucontext_t context{};
    int saved_errno = 0;
};

/**
 * @brief 在借用栈上建立无参数入口，上下文不拥有栈存储。
 * @pre stack 指向至少 size 个可写字节，须覆盖上下文执行寿命；entry
 * 不得直接返回。
 * @throws std::system_error getcontext 失败。
 */
void make_context(NativeContext &, void *stack, size_t size, void (*entry)());

/**
 * @brief 保存当前上下文及 errno 并恢复目标；再次切回时恢复当前 errno。
 * @pre 两个上下文及其栈均有效，只在所属工作线程调用。
 * @throws std::system_error swapcontext 失败。
 */
void switch_context(NativeContext &from, NativeContext &to);
char *stack_pointer(const NativeContext &);
} // namespace detail
} // namespace zco

#endif
