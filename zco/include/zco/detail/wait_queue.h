#ifndef ZLYNX_ZCO_INCLUDE_ZCO_DETAIL_WAIT_QUEUE_H_
#define ZLYNX_ZCO_INCLUDE_ZCO_DETAIL_WAIT_QUEUE_H_

#include "zco/coroutine.h"
#include <list>

namespace zco {
namespace detail {
/**
 * @brief 为公开模板提供等待桥接；调用方须用同一互斥量保护谓词和队列，
 * 并在 ticket.wait() 前释放该锁，避免阻塞完成等待的一方。
 */
class WaitTicket {
  public:
    WaitTicket();

    WakeToken token() const { return WakeToken(state_); }

    WaitOutcome wait(Deadline deadline = {}) const;

    explicit WaitTicket(std::shared_ptr<WaitState> state)
        : state_(std::move(state)) {}

  private:
    friend class WaitQueue;
    std::shared_ptr<WaitState> state_;
};

/**
 * @brief 不拥有等待状态的登记队列，用于在共享谓词变化时唤醒等待者。
 *
 * 本类不加锁，调用方须用同一互斥量保护谓词及全部队列操作。
 * 析构仅解除登记，不完成等待；拥有者须先安排等待方退出。
 */
class WaitQueue {
  public:
    WaitQueue() = default;
    ~WaitQueue();
    WaitQueue(const WaitQueue &) = delete;
    WaitQueue &operator=(const WaitQueue &) = delete;

    /**
     * @brief 登记一次与当前协程或普通线程绑定的等待。
     * @return 拥有等待状态的票据；开始等待前须释放谓词锁。
     */
    WaitTicket add();

    /**
     * @brief 解除本队列中的票据登记，不改变等待结果；已移除时无操作。
     */
    void remove(const WaitTicket &ticket);

    /**
     * @brief 移除失效或已完成项，并尝试完成首个仍在等待的登记项。
     * @return 本次完成一个等待时返回 true；没有可完成项时返回 false。
     */
    bool complete_one(WaitOutcome outcome = WaitOutcome::ready);
    void complete_all(WaitOutcome outcome = WaitOutcome::ready);

  private:
    using Entries = std::list<std::weak_ptr<WaitState>>;
    void erase(Entries::iterator entry);
    void prune_expired();
    Entries entries_;

    // 有界清理的续扫位置；摘除当前节点时同步推进，防止迭代器失效。
    Entries::iterator cleanup_ = entries_.end();
};
} // namespace detail
} // namespace zco

#endif
