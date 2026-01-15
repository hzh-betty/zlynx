#ifndef ZCOROUTINE_CONTEXT_H_
#define ZCOROUTINE_CONTEXT_H_

#include <ucontext.h>

namespace zcoroutine {

/**
 * @brief 上下文封装类
 * 封装ucontext_t相关操作，从Fiber中解耦上下文管理
 * 提供静态方法封装getcontext、makecontext、swapcontext
 */
class Context {
public:
  Context() = default;
  ~Context() = default;

  /**
   * @brief 获取ucontext_t指针
   * @return ucontext_t指针
   */
  ucontext_t *get() { return &ctx_; }
  const ucontext_t *get() const { return &ctx_; }

  /**
   * @brief 创建上下文
   * @param stack_ptr 栈指针
   * @param stack_size 栈大小
   * @param func 上下文执行的函数
   */
  void make_context(void *stack_ptr, size_t stack_size, void (*func)());

  /**
   * @brief 切换上下文（静态方法）
   * @param from_ctx 源上下文
   * @param to_ctx 目标上下文
   * @return 成功返回0，失败返回-1
   */
  static int swap_context(Context *from_ctx, Context *to_ctx);

  /**
   * @brief 获取当前上下文
   * @return 成功返回0，失败返回-1
   */
  int get_context();

  /**
   * @brief 从保存的上下文中获取栈指针(rsp/sp)
   * @return 栈指针值
   * @note 只有在swapcontext保存上下文后才有效
   */
  void *get_stack_pointer() const;

private:
  ucontext_t ctx_{}; // 底层上下文结构
};

} // namespace zcoroutine

#endif // ZCOROUTINE_CONTEXT_H_
