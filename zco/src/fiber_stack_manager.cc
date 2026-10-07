#include "zco/internal/fiber_stack_manager.h"

#include "zco/internal/fiber.h"
#include "zco/zco_logger.h"
#include <cstring>

namespace zco {
namespace {
#if defined(__x86_64__)
constexpr size_t kStackRedZoneBytes = 128;
#else
constexpr size_t kStackRedZoneBytes = 0;
#endif
} // 命名空间

FiberStackManager::FiberStackManager(int scheduler_id, size_t stack_count,
                                     size_t stack_size)
    : scheduler_id_(scheduler_id), stacks_(stack_count, stack_size) {}

void *FiberStackManager::data(size_t slot) { return stacks_.data(slot); }
size_t FiberStackManager::size(size_t slot) const { return stacks_.size(slot); }
size_t FiberStackManager::count() const { return stacks_.count(); }
size_t FiberStackManager::next_slot() {
    return count() == 0
               ? 0
               : next_slot_.fetch_add(1, std::memory_order_relaxed) % count();
}
char *FiberStackManager::acquire_snapshot(size_t size, size_t *capacity,
                                          uint8_t *bucket) {
    return snapshots_.acquire(size, capacity, bucket);
}
void FiberStackManager::release_snapshot(char *buffer, uint8_t bucket,
                                         size_t capacity) {
    snapshots_.release(buffer, bucket, capacity);
}

void FiberStackManager::save(Fiber *fiber) {
    if (!fiber || !fiber->use_shared_stack()) {
        return;
    }

    const size_t stack_slot = fiber->stack_slot();
    const size_t stack_size = stacks_.size(stack_slot);
    void *stack_data = stacks_.data(stack_slot);
    if (stack_size == 0 || !stack_data) {
        return;
    }

    const uintptr_t stack_bottom = reinterpret_cast<uintptr_t>(stack_data);
    const uintptr_t stack_top = stack_bottom + stack_size;
    const uintptr_t stack_sp =
        reinterpret_cast<uintptr_t>(fiber->context()->get_stack_pointer());
    if (stack_sp == 0) {
        ZCO_LOG_WARN("shared stack save skipped, unsupported architecture");
        return;
    }

    // 共享栈保存的是当前活跃栈帧区间，而不是整块栈内存。通过当前 SP 计
    // 算已使用范围，再把这段内容快照到 Fiber 自己的保存区里。
    if (stack_sp < stack_bottom || stack_sp > stack_top) {
        ZCO_LOG_WARN("shared stack save failed, sp out of range, "
                     "sched_id={}, fiber_id={}, sp={}",
                     scheduler_id_, fiber->id(), stack_sp);
        return;
    }

    uintptr_t save_begin = stack_sp;
    if (kStackRedZoneBytes != 0) {
        const uintptr_t red_zone_begin =
            stack_sp >= stack_bottom + kStackRedZoneBytes
                ? stack_sp - kStackRedZoneBytes
                : stack_bottom;
        save_begin = red_zone_begin;
    }

    const size_t used = stack_top - save_begin;
    fiber->save_stack_data(reinterpret_cast<const char *>(save_begin), used);
    ZCO_LOG_DEBUG("shared stack saved, sched_id={}, fiber_id={}, used_bytes={}",
                  scheduler_id_, fiber->id(), used);
}

void FiberStackManager::restore(const Fiber::ptr &fiber) {
    if (!fiber->has_saved_stack()) {
        return;
    }

    const size_t stack_slot = fiber->stack_slot();
    const size_t stack_size = stacks_.size(stack_slot);
    void *stack_data = stacks_.data(stack_slot);
    if (stack_size == 0 || !stack_data) {
        return;
    }

    const size_t used = fiber->saved_stack_size();
    if (used > stack_size) {
        ZCO_LOG_WARN("shared stack restore failed, snapshot too large, "
                     "sched_id={}, fiber_id={}, used={}, stack={}",
                     scheduler_id_, fiber->id(), used, stack_size);
        return;
    }

    // 恢复时把保存区直接拷回共享栈尾部，保持调用栈相对栈顶的布局不变，
    // 这样 ucontext 恢复后协程就能继续从上次让出的精确位置运行。
    char *dst = reinterpret_cast<char *>(stack_data) + (stack_size - used);
    std::memcpy(dst, fiber->saved_stack_data(), used);
    ZCO_LOG_DEBUG(
        "shared stack restored, sched_id={}, fiber_id={}, used_bytes={}",
        scheduler_id_, fiber->id(), used);
}

void FiberStackManager::prepare(const Fiber::ptr &fiber) {
    if (!fiber || !fiber->use_shared_stack()) {
        return;
    }

    const size_t stack_slot = fiber->stack_slot();
    // 共享栈模型下，同一时刻一个 stack_slot 只能被一个 Fiber 占用。
    // 如果这里切换到另一个 Fiber，就要先把旧 Fiber 的现场保存下来，
    // 再把新 Fiber 的快照恢复到该槽位。
    const SharedStackOwner owner = stacks_.occupy_fiber(stack_slot);
    if (owner.fiber == fiber.get() && owner.fiber_id == fiber->id()) {
        return;
    }

    if (owner.fiber && owner.fiber->id() == owner.fiber_id &&
        owner.fiber->state() != Fiber::State::kDone) {
        save(owner.fiber);
    }

    stacks_.set_occupy_fiber(stack_slot, fiber.get(), fiber->id());
    restore(fiber);
}

void FiberStackManager::release(const std::shared_ptr<Fiber> &fiber) {
    fiber->clear_saved_stack();
    if (fiber->use_shared_stack()) {
        const SharedStackOwner owner =
            stacks_.occupy_fiber(fiber->stack_slot());
        if (owner.fiber == fiber.get() && owner.fiber_id == fiber->id()) {
            stacks_.set_occupy_fiber(fiber->stack_slot(), nullptr, 0);
        }
    }
}

} // 命名空间 zco
