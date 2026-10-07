#ifndef ZCO_INTERNAL_FIBER_STACK_MANAGER_H_
#define ZCO_INTERNAL_FIBER_STACK_MANAGER_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "zco/internal/shared_stack_buffer.h"
#include "zco/internal/snapshot_buffer_pool.h"

namespace zco {
class Fiber;

// 管理共享栈槽位、占用者及快照存储的生命周期。
// 不依赖调度器或全局运行时。
class FiberStackManager {
  public:
    FiberStackManager(int scheduler_id, size_t stack_count, size_t stack_size);

    void *data(size_t slot);
    size_t size(size_t slot) const;
    size_t count() const;
    size_t next_slot();

    char *acquire_snapshot(size_t size, size_t *capacity, uint8_t *bucket);
    void release_snapshot(char *buffer, uint8_t bucket, size_t capacity);

    void save(Fiber *fiber);
    void restore(const std::shared_ptr<Fiber> &fiber);
    void prepare(const std::shared_ptr<Fiber> &fiber);
    void release(const std::shared_ptr<Fiber> &fiber);

  private:
    SharedStackBuffer *stack_at(size_t slot);

    int scheduler_id_;
    std::atomic<size_t> next_slot_{0};
    SnapshotBufferPool snapshots_;
    std::vector<SharedStackBuffer> stacks_;
};
} // 命名空间 zco

#endif
