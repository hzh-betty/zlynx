#ifndef ZLYNX_ZCO_SRC_WAIT_TIMER_QUEUE_H_
#define ZLYNX_ZCO_SRC_WAIT_TIMER_QUEUE_H_

#include "zco/coroutine.h"
#include <map>
#include <unordered_map>

namespace zco {
namespace detail {
using TimerId = uint64_t;

/**
 * @brief 工作线程独享的截止时间索引，同时支持按时间到期和按 ID 取消。
 *
 * 不提供并发保护；唤醒令牌弱引用等待状态，不因定时器延长任务寿命。
 */
class TimerQueue {
  public:
    /**
     * @brief 为有限截止时间登记超时唤醒。
     * @return 有限截止时间的登记 ID；无限等待不登记并返回 0。
     */
    TimerId add(Deadline deadline, WakeToken token);

    /**
     * @brief 移除定时器，不完成其等待状态。
     * @return 找到并移除时返回 true；0、已到期或已取消的 ID 返回 false。
     */
    bool cancel(TimerId id);

    /**
     * @brief 移除截止时间不晚于 now 的项，并尝试将其等待完成为超时。
     */
    void expire(Deadline::TimePoint now);
    Deadline next_deadline() const;

    size_t size() const { return entries_.size(); }

  private:
    struct Entry {
        TimerId id;
        WakeToken token;
    };

    std::multimap<Deadline::TimePoint, Entry> entries_;

    // ID 索引借用时间序列中的迭代器，插入、取消和到期必须同时维护两者。
    std::unordered_map<TimerId, decltype(entries_)::iterator> index_;
    TimerId next_ = 0;
};
} // namespace detail
} // namespace zco

#endif
