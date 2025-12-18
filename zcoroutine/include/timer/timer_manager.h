#ifndef ZCOROUTINE_TIMER_MANAGER_H_
#define ZCOROUTINE_TIMER_MANAGER_H_

#include <set>
#include <mutex>
#include <vector>
#include <functional>
#include "timer/timer.h"

namespace zcoroutine {

/**
 * @brief 定时器管理器
 * 管理所有定时器，使用std::set按时间排序
 */
class TimerManager {
public:
    /**
     * @brief 添加定时器
     * @param timeout 超时时间（毫秒）
     * @param callback 回调函数
     * @param recurring 是否循环
     * @return 定时器智能指针
     */
    Timer::ptr add_timer(uint64_t timeout, std::function<void()> callback, bool recurring = false);

    /**
     * @brief 获取下一个定时器的超时时间
     * @return 超时时间（毫秒），如果没有定时器返回-1
     */
    int get_next_timeout();

    /**
     * @brief 获取所有到期的定时器回调
     * @return 到期定时器的回调函数列表
     */
    std::vector<std::function<void()>> list_expired_callbacks();

private:
    // 定时器比较器（按next_time_排序）
    struct TimerComparator {
        bool operator()(const Timer::ptr& lhs, const Timer::ptr& rhs) const {
            if (!lhs || !rhs) {
                return lhs < rhs;
            }
            if (lhs->next_time_ != rhs->next_time_) {
                return lhs->next_time_ < rhs->next_time_;
            }
            return lhs.get() < rhs.get();
        }
    };

    std::set<Timer::ptr, TimerComparator> timers_;  // 定时器集合
    mutable std::mutex mutex_;                       // 互斥锁
};

} // namespace zcoroutine

#endif // ZCOROUTINE_TIMER_MANAGER_H_
