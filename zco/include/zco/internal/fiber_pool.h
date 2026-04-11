#ifndef ZCO_INTERNAL_FIBER_POOL_H_
#define ZCO_INTERNAL_FIBER_POOL_H_

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>

#include "zco/internal/fiber.h"
#include "zco/internal/noncopyable.h"

namespace zco {

class FiberPool : public NonCopyable {
  public:
    explicit FiberPool(size_t max_size);

    Fiber::ptr acquire();

    void recycle(const Fiber::ptr &fiber);

    void clear();

    size_t size() const;

  private:
    const size_t max_size_;
    mutable std::mutex mutex_;
    std::deque<Fiber::ptr> fibers_;
};

} // namespace zco

#endif // ZCO_INTERNAL_FIBER_POOL_H_