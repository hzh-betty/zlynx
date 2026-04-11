#include "zco/internal/fiber_handle_registry.h"

namespace zco {

// FiberHandleRegistry 为裸句柄提供“稳定映射”：
// - 外部接口返回的 handle 只是 uint64_t 编码，不直接暴露 Fiber 指针。
// - 通过原子外部句柄 + 互斥 map 双重保障，避免对象复用后出现 stale handle。

FiberHandleRegistry::FiberHandleRegistry() : mutex_(), handle_map_() {
    // 短任务压测下句柄注册/注销频繁，预留容量可减少rehash与缓存抖动。
    handle_map_.reserve(8192);
}

void FiberHandleRegistry::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    handle_map_.clear();
}

uint64_t FiberHandleRegistry::register_fiber(const Fiber::ptr &fiber,
                                             uint64_t handle_id) {
    if (!fiber || handle_id == 0) {
        return 0;
    }

    uint64_t effective_handle_id = 0;
    // 先占用 Fiber 上的原子句柄槽，再进入全局表，保证句柄唯一归属。
    if (!fiber->try_set_external_handle_id(handle_id, &effective_handle_id)) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    handle_map_[effective_handle_id] = fiber;
    return effective_handle_id;
}

uint64_t FiberHandleRegistry::unregister_fiber(Fiber *fiber) {
    if (!fiber) {
        return 0;
    }

    // 先从 Fiber 实体上撤销句柄，再删除全局映射，防止后续重复注销误命中。
    const uint64_t handle_id = fiber->clear_external_handle_id();
    if (handle_id == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    handle_map_.erase(handle_id);
    return handle_id;
}

Fiber::ptr FiberHandleRegistry::find_by_handle(uint64_t handle_id) const {
    if (handle_id == 0) {
        return nullptr;
    }

    // 查表命中后直接返回 shared_ptr，确保调用方拿到的是受管生命周期。
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = handle_map_.find(handle_id);
    if (it == handle_map_.end()) {
        return nullptr;
    }
    return it->second;
}

bool FiberHandleRegistry::try_get_handle_id(const Fiber *fiber,
                                            uint64_t *handle_id) const {
    if (!fiber || !handle_id) {
        return false;
    }

    // 仅在句柄已经分配过时返回，避免把“尚未注册”的 fiber 误当成外部可恢复对象。
    const uint64_t id = fiber->external_handle_id();
    if (id == 0) {
        return false;
    }

    *handle_id = id;
    return true;
}

} // namespace zco