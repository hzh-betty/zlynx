#include "zcoroutine/wait_group.h"

#include <stdexcept>

#include "zcoroutine/log.h"

namespace zcoroutine {

// WaitGroup 语义与 Go 类似：
// - add(n) 增加未完成任务数。
// - done() 标记单个任务完成。
// - wait() 阻塞直到计数归零。
// 内部使用原子计数 + Event 保证线程/协程两种上下文可用。

WaitGroup::WaitGroup(uint32_t count)
    : count_(static_cast<int64_t>(count)), done_event_(true, count == 0) {
    ZCOROUTINE_LOG_DEBUG("wait_group created, initial_count={}", count);
}

void WaitGroup::add(uint32_t count) {
    // add 与 done 可并发执行，因此计数器使用 fetch_add/fetch_sub 保证原子性。
    const int64_t delta = static_cast<int64_t>(count);
    const int64_t previous = count_.fetch_add(delta, std::memory_order_acq_rel);
    ZCOROUTINE_LOG_DEBUG("wait_group add, delta={}, previous={}, current={}",
                         delta, previous, previous + delta);
    if (previous == 0 && delta > 0) {
        // 从 0 变为正值时，说明需要重新进入等待态。
        // 注意：调用方应避免与 wait() 产生业务层竞态（与 Go WaitGroup
        // 使用约束一致）。
        done_event_.reset();
    }
}

void WaitGroup::done() {
    const int64_t previous = count_.fetch_sub(1, std::memory_order_acq_rel);
    if (previous <= 0) {
        // done 次数超过 add 累计值属于调用错误，恢复计数并抛异常。
        count_.fetch_add(1, std::memory_order_acq_rel);
        ZCOROUTINE_LOG_ERROR(
            "wait_group done failed, counter would become negative");
        throw std::runtime_error("WaitGroup counter becomes negative");
    }

    if (previous == 1) {
        // 从 1 递减到 0，触发等待者全部可继续。
        ZCOROUTINE_LOG_DEBUG("wait_group reached zero, signal waiters");
        done_event_.signal();
    }
}

void WaitGroup::wait() {
    // wait 支持线程与协程两种上下文，具体行为由 Event::wait 决定。
    // 若计数已为 0，done_event_ 会立即返回，不发生阻塞。
    ZCOROUTINE_LOG_DEBUG("wait_group wait begin");
    (void)done_event_.wait();
    ZCOROUTINE_LOG_DEBUG("wait_group wait end");
}

} // namespace zcoroutine
