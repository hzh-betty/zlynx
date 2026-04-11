#ifndef ZCO_INTERNAL_CONTEXT_H_
#define ZCO_INTERNAL_CONTEXT_H_

#include <ucontext.h>

#include <cstddef>

namespace zco {

/**
 * @brief 协程上下文类
 * @details 封装 ucontext_t，提供创建上下文、切换上下文等功能。
 */
class Context {
  public:
    Context() = default;
    ~Context() = default;

    /**
     * @brief 获取上下文
     * @return 上下文指针
     */
    ucontext_t *get() { return &ctx_; }
    const ucontext_t *get() const { return &ctx_; }

    /**
     * @brief 创建上下文
     * @param stack_ptr 协程栈指针
     * @param stack_size 协程栈大小
     * @param func 协程入口函数
     * @param link 可选的链接上下文，默认为 nullptr
     */
    void make_context(void *stack_ptr, size_t stack_size, void (*func)(),
                      ucontext_t *link = nullptr);

    /**
     * @brief 切换上下文
     * @param from_ctx 当前上下文
     * @param to_ctx 目标上下文
     * @return 成功返回 0，失败返回 -1 并设置 errno
     */
    static int swap_context(Context *from_ctx, Context *to_ctx);

    /**
     * @brief 获取上下文
     * @return 上下文指针
     */
    int get_context();

    /**
     * @brief 获取当前栈指针
     * @return 当前栈指针
     */
    void *get_stack_pointer() const;

  private:
    ucontext_t ctx_{};
};

} // namespace zco

#endif // ZCO_INTERNAL_CONTEXT_H_