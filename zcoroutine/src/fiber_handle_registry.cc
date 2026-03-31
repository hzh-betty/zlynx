#include "zcoroutine/internal/fiber_handle_registry.h"

namespace zcoroutine {

FiberHandleRegistry::FiberHandleRegistry() : mutex_(), handle_map_(), reverse_map_() {}

void FiberHandleRegistry::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  handle_map_.clear();
  reverse_map_.clear();
}

void FiberHandleRegistry::register_fiber(const std::shared_ptr<Fiber>& fiber, uint64_t handle_id) {
  if (!fiber || handle_id == 0) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const Fiber* key = fiber.get();
  auto it = reverse_map_.find(key);
  if (it != reverse_map_.end()) {
    handle_map_[it->second] = fiber;
    return;
  }

  reverse_map_[key] = handle_id;
  handle_map_[handle_id] = fiber;
}

void FiberHandleRegistry::unregister_fiber(const Fiber* fiber) {
  if (!fiber) {
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = reverse_map_.find(fiber);
  if (it == reverse_map_.end()) {
    return;
  }

  handle_map_.erase(it->second);
  reverse_map_.erase(it);
}

std::shared_ptr<Fiber> FiberHandleRegistry::find_by_handle(uint64_t handle_id) const {
  if (handle_id == 0) {
    return nullptr;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = handle_map_.find(handle_id);
  if (it == handle_map_.end()) {
    return nullptr;
  }
  return it->second;
}

bool FiberHandleRegistry::try_get_handle_id(const Fiber* fiber, uint64_t* handle_id) const {
  if (!fiber || !handle_id) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  auto it = reverse_map_.find(fiber);
  if (it == reverse_map_.end()) {
    return false;
  }

  *handle_id = it->second;
  return true;
}

}  // namespace zcoroutine