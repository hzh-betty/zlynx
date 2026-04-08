#include "zcoroutine/internal/fiber_handle_registry.h"

namespace zcoroutine {

FiberHandleRegistry::FiberHandleRegistry() : mutex_(), handle_map_() {}

void FiberHandleRegistry::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  handle_map_.clear();
}

uint64_t FiberHandleRegistry::register_fiber(const std::shared_ptr<Fiber>& fiber,
                                             uint64_t handle_id) {
  if (!fiber || handle_id == 0) {
    return 0;
  }

  uint64_t effective_handle_id = 0;
  if (!fiber->try_set_external_handle_id(handle_id, &effective_handle_id)) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  handle_map_[effective_handle_id] = fiber;
  return effective_handle_id;
}

uint64_t FiberHandleRegistry::unregister_fiber(Fiber* fiber) {
  if (!fiber) {
    return 0;
  }

  const uint64_t handle_id = fiber->clear_external_handle_id();
  if (handle_id == 0) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  handle_map_.erase(handle_id);
  return handle_id;
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

  const uint64_t id = fiber->external_handle_id();
  if (id == 0) {
    return false;
  }

  *handle_id = id;
  return true;
}

}  // namespace zcoroutine