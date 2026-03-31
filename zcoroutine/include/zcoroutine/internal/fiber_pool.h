#ifndef ZCOROUTINE_INTERNAL_FIBER_POOL_H_
#define ZCOROUTINE_INTERNAL_FIBER_POOL_H_

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>

#include "zcoroutine/internal/fiber.h"
#include "zcoroutine/internal/noncopyable.h"

namespace zcoroutine {

class FiberPool : public NonCopyable {
 public:
  explicit FiberPool(size_t max_size);

  std::shared_ptr<Fiber> acquire();

  void recycle(const std::shared_ptr<Fiber>& fiber);

  void clear();

  size_t size() const;

 private:
  const size_t max_size_;
  mutable std::mutex mutex_;
  std::deque<std::shared_ptr<Fiber>> fibers_;
};

}  // namespace zcoroutine

#endif  // ZCOROUTINE_INTERNAL_FIBER_POOL_H_