#ifndef ZCO_INTERNAL_SHARED_STACK_BUFFER_H_
#define ZCO_INTERNAL_SHARED_STACK_BUFFER_H_

#include <cstddef>
#include <vector>

#include "zco/internal/noncopyable.h"

namespace zco {

class Fiber;

class SharedStackBuffer : public NonCopyable {
  public:
    explicit SharedStackBuffer(size_t stack_size = 0);
    ~SharedStackBuffer();

    SharedStackBuffer(SharedStackBuffer &&other) noexcept;
    SharedStackBuffer &operator=(SharedStackBuffer &&other) noexcept;

    char *data();

    const char *data() const;

    char *stack_bp();

    const char *stack_bp() const;

    size_t size() const;

    Fiber *occupy_fiber() const;

    void set_occupy_fiber(Fiber *fiber);

  private:
    char *stack_buffer_;
    char *stack_bp_;
    size_t stack_size_;
    Fiber *occupy_fiber_;
};

class SharedStackPool : private NonCopyable {
  public:
    SharedStackPool(size_t stack_count, size_t stack_size);

    void *data(size_t stack_slot);

    size_t size(size_t stack_slot) const;

    size_t count() const;

  private:
    std::vector<SharedStackBuffer> stacks_;
};

} // namespace zco

#endif // ZCO_INTERNAL_SHARED_STACK_BUFFER_H_