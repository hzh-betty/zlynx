#ifndef ZCO_INTERNAL_SHARED_STACK_BUFFER_H_
#define ZCO_INTERNAL_SHARED_STACK_BUFFER_H_

#include <cstddef>
#include <vector>

#include "zco/internal/noncopyable.h"

namespace zco {

class Fiber;

/**
 * @brief 共享栈缓冲区
 * @details
 * - 每个 SharedStackBuffer 代表一个可被 Fiber 占用的栈空间。
 * - 通过 SharedStackPool 管理多个 SharedStackBuffer，实现协程栈的复用。
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

    /**
     * @brief 获取栈基址指针
     * @return 栈基址指针
     */
    char *stack_bp();

    const char *stack_bp() const;

    size_t size() const;

    /**
     * @brief 获取占用的 Fiber 对象
     * @return 占用的 Fiber 对象，未被占用时返回 nullptr
     */
    Fiber *occupy_fiber() const;

    void set_occupy_fiber(Fiber *fiber);

  private:
    char *stack_buffer_;
    char *stack_bp_;
    size_t stack_size_;
    Fiber *occupy_fiber_;
};

/**
 * @brief 共享栈池
 * @details 管理多个 SharedStackBuffer，实现协程栈的复用。
 */
class SharedStackPool : private NonCopyable {
  public:
    SharedStackPool(size_t stack_count, size_t stack_size);

    void *data(size_t stack_slot);

    /**
     * @brief 获取指定栈槽的栈基址指针
     * @param stack_slot 栈槽索引
     * @return 栈基址指针
     */
    size_t size(size_t stack_slot) const;

    size_t count() const;

  private:
    std::vector<SharedStackBuffer> stacks_;
};

} // namespace zco

#endif // ZCO_INTERNAL_SHARED_STACK_BUFFER_H_