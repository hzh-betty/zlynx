#ifndef ZLYNX_ZCO_SRC_WAIT_WAIT_STATE_H_
#define ZLYNX_ZCO_SRC_WAIT_WAIT_STATE_H_

#include "zco/coroutine.h"
#include <condition_variable>
#include <list>
#include <mutex>
#include <vector>

namespace zco {
namespace detail {
class WaitQueue;

class CompletionEndpoint {
  public:
    virtual ~CompletionEndpoint() = default;
    virtual void notify(WaitId id) = 0;
};

/**
 * @brief 一次等待的共享状态，协调就绪、超时、取消和关闭之间的竞争。
 *
 * mutex_ 保护终态；首个完成者决定结果。普通线程由条件变量唤醒，
 * 协程由弱引用端点通知所属工作线程，状态本身不延长运行时生命周期。
 */
class WaitState {
  public:
    explicit WaitState(WaitId id,
                       std::weak_ptr<CompletionEndpoint> endpoint = {})
        : id_(id), endpoint_(std::move(endpoint)) {}

    /**
     * @brief 尝试发布终态并通知等待方。
     * @return 本次首次完成状态时返回 true；已完成时返回 false 并保留原结果。
     */
    bool complete(WaitOutcome outcome);
    bool completed() const;

    /**
     * @brief 查询已发布的终态。
     * @throws std::logic_error 等待尚未完成。
     */
    WaitOutcome outcome() const;

    /**
     * @brief 阻塞普通线程，直到共享状态完成或尝试以超时完成。
     *
     * @return
     * 竞争中最先发布的终态；到达截止时间也可能返回已抢先发布的就绪结果。
     */
    WaitOutcome wait_thread(Deadline deadline);

    WaitId id() const { return id_; }

  private:
    friend class WaitQueue;

    // 仅在持有注册队列的谓词互斥量时访问。
    WaitQueue *queue_ = nullptr;
    std::list<std::weak_ptr<WaitState>>::iterator queue_entry_;
    WaitId id_;
    std::weak_ptr<CompletionEndpoint> endpoint_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;

    // outcome_ 仅在 completed_ 为 true 后有效，两者由 mutex_ 一并保护。
    bool completed_ = false;
    WaitOutcome outcome_ = WaitOutcome::ready;
};

// 由运行时当前执行入口提供，不经过服务查找。
std::shared_ptr<WaitState> prepare_wait();
WaitOutcome park_wait(const std::shared_ptr<WaitState> &, Deadline);
} // namespace detail
} // namespace zco

#endif
