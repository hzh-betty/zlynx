#ifndef ZCO_INTERNAL_FIBER_POOL_H_
#define ZCO_INTERNAL_FIBER_POOL_H_

#include <cstddef>
#include <deque>
#include <mutex>

#include "zco/internal/fiber.h"
#include "zco/internal/noncopyable.h"

namespace zco {

class FiberPool : public NonCopyable {
  public:
    explicit FiberPool(size_t max_size);

    /**
     * @brief 获取 Fiber 对象
     * @return 可用的 Fiber 对象，如果池中没有可用对象且未达到 max_size
     * 则创建新对象，否则返回 nullptr
     */
    Fiber::ptr acquire();

    /**
     * @brief 回收 Fiber 对象
     * @param fiber 待回收的 Fiber 对象
     */
    void recycle(const Fiber::ptr &fiber);

    /**
     * @brief 清空 Fiber 池，销毁所有 Fiber 对象
     */
    void clear();

    size_t size() const;

  private:
    const size_t max_size_;
    mutable std::mutex mutex_;
    std::deque<Fiber::ptr> fibers_;
};

} // namespace zco

#endif // ZCO_INTERNAL_FIBER_POOL_H_