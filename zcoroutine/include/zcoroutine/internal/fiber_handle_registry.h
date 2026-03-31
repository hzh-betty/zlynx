#ifndef ZCOROUTINE_INTERNAL_FIBER_HANDLE_REGISTRY_H_
#define ZCOROUTINE_INTERNAL_FIBER_HANDLE_REGISTRY_H_

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "zcoroutine/internal/fiber.h"
#include "zcoroutine/internal/noncopyable.h"

namespace zcoroutine {

/**
 * @brief Fiber 外部句柄注册表。
 * @details 维护 handle_id <-> Fiber 的双向映射，避免对象复用导致 stale handle 误命中。
 */
class FiberHandleRegistry : public NonCopyable {
 public:
  FiberHandleRegistry();

  void clear();

  void register_fiber(const std::shared_ptr<Fiber>& fiber, uint64_t handle_id);

  void unregister_fiber(const Fiber* fiber);

  std::shared_ptr<Fiber> find_by_handle(uint64_t handle_id) const;

  bool try_get_handle_id(const Fiber* fiber, uint64_t* handle_id) const;

 private:
  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, std::shared_ptr<Fiber>> handle_map_;
  std::unordered_map<const Fiber*, uint64_t> reverse_map_;
};

}  // namespace zcoroutine

#endif  // ZCOROUTINE_INTERNAL_FIBER_HANDLE_REGISTRY_H_