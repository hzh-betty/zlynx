#ifndef ZCO_INTERNAL_FIBER_HANDLE_REGISTRY_H_
#define ZCO_INTERNAL_FIBER_HANDLE_REGISTRY_H_

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "zco/internal/fiber.h"
#include "zco/internal/noncopyable.h"

namespace zco {

/**
 * @brief Fiber 外部句柄注册表。
 * @details 维护 handle_id <-> Fiber 的双向映射，避免对象复用导致 stale handle
 * 误命中。
 */
class FiberHandleRegistry : public NonCopyable {
  public:
    FiberHandleRegistry();

    /**
     * @brief 清空注册表
     */
    void clear();

    /**
     * @brief 注册 Fiber，返回唯一 handle_id
     * @param fiber 待注册的 Fiber 对象
     * @return 成功返回非零 handle_id，失败返回 0
     */
    uint64_t register_fiber(const Fiber::ptr &fiber, uint64_t handle_id);

    /**
     * @brief 注销 Fiber，释放 handle_id
     * @param fiber 待注销的 Fiber 对象
     * @return 成功返回对应的 handle_id，失败返回 0
     */
    uint64_t unregister_fiber(Fiber *fiber);

    /**
     * @brief 根据 handle_id 查找 Fiber
     * @param handle_id 待查找的 handle_id
     * @return 成功返回对应的 Fiber 对象，失败返回 nullptr
     */
    Fiber::ptr find_by_handle(uint64_t handle_id) const;

    /**
     * @brief 根据 Fiber 查找 handle_id
     * @param fiber 待查找的 Fiber 对象
     * @return 成功返回对应的 handle_id，失败返回 0
     */
    bool try_get_handle_id(const Fiber *fiber, uint64_t *handle_id) const;

  private:
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, Fiber::ptr> handle_map_;
};

} // namespace zco

#endif // ZCO_INTERNAL_FIBER_HANDLE_REGISTRY_H_