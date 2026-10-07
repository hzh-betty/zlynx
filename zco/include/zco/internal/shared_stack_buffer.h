/**
 * @file shared_stack_buffer.h
 * @brief shared_stack_buffer 定义。
 * @author hzh-betty
 */

#ifndef ZCO_INTERNAL_SHARED_STACK_BUFFER_H_
#define ZCO_INTERNAL_SHARED_STACK_BUFFER_H_

#include <cstddef>

#include "zco/internal/noncopyable.h"

namespace zco {

class Fiber;

/**
 * @brief 共享栈所有者
 * @details 表示占用共享栈的 Fiber 对象及其 ID。
 */
struct SharedStackOwner {
    Fiber *fiber;
    int fiber_id;

    SharedStackOwner() : fiber(nullptr), fiber_id(0) {}
    SharedStackOwner(Fiber *owner_fiber, int owner_fiber_id)
        : fiber(owner_fiber), fiber_id(owner_fiber_id) {}
};

/**
 * @brief 共享栈缓冲区
 * @details
 * - 每个 SharedStackBuffer 代表一个可被 Fiber 占用的栈空间。
 * - 通过 FiberStackManager 管理多个 SharedStackBuffer，实现协程栈的复用。
 * - 协程切换时，保存/恢复占用的 SharedStackBuffer 数据，实现栈内容的迁移。
 */
class SharedStackBuffer : public NonCopyable {
  public:
    explicit SharedStackBuffer(size_t stack_size = 0);
    ~SharedStackBuffer();

    SharedStackBuffer(SharedStackBuffer &&other) noexcept;
    SharedStackBuffer &operator=(SharedStackBuffer &&other) noexcept;

    /**
     * @brief 获取栈数据指针
     * @return 栈数据指针
     */
    char *data();

    const char *data() const;

    size_t size() const;

    /**
     * @brief 获取占用的 Fiber 对象
     * @return 占用的 Fiber 对象，未被占用时返回 nullptr
     */
    SharedStackOwner occupy_fiber() const;

    void set_occupy_fiber(Fiber *fiber, int fiber_id);

  private:
    char *stack_buffer_;
    size_t stack_size_;
    SharedStackOwner occupy_fiber_;
};

} // namespace zco

#endif // ZCO_INTERNAL_SHARED_STACK_BUFFER_H_
