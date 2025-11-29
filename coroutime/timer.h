#ifndef ZLYNX_TIMER_H
#define ZLYNX_TIMER_H

#include <set>
#include <memory>
#include <functional>
#include <atomic>
#include <chrono>


#include "spinlock.hpp"

namespace zlynx
{
    class TimerManager;

    class Timer : public std::enable_shared_from_this<Timer>
    {
    public:
        using ptr = std::shared_ptr<Timer>;
        using Callback = std::function<void()>;

        // 取消定时器
        bool cancel();

        // 刷新定时器到当前时间
        bool refresh();

        bool erase_timer();

        // 重置定时器
        bool reset(uint64_t ms, bool from_now);

    private:
        // 小根堆比较器
        struct Comparator
        {
            bool operator()(const Timer::ptr &lhs, const Timer::ptr &rhs) const
            {
                if (!lhs && !rhs) return true;
                if (!lhs) return false;
                if (!rhs) return true;

                if (lhs->next_ms_ == rhs->next_ms_)
                {
                    return lhs.get() < rhs.get();
                }
                return lhs->next_ms_ < rhs->next_ms_;
            }
        };

        friend class TimerManager;

        Timer(uint64_t internal_ms, Callback callback, bool recurring, TimerManager *manager);

        explicit Timer(uint64_t internal_ms);

        uint64_t internal_ms_ = 0;
        bool recurring_ = false;
        std::chrono::time_point<std::chrono::system_clock> next_ms_;
        Callback callback_;
        TimerManager *manager_ = nullptr;
    };

    class TimerManager
    {
    public:
        friend class Timer;
        using MutexType = SpinLock;

        TimerManager() = default;

        virtual ~TimerManager();

        // 添加定时器
        Timer::ptr add_timer(uint64_t ms, const Timer::Callback &cb,
                            bool recurring = false);

        Timer::ptr add_condition_timer(uint64_t ms, const Timer::Callback &cb,
                                     const std::weak_ptr<void> &weak_cond,
                                     bool recurring = false);

        uint64_t get_next_expire_time();

        // 扫描并执行到期的定时器回调函数
        void list_expired_callbacks(std::vector<Timer::Callback> &cbs);

        bool has_timer() const;

    protected:
        virtual void on_timer_inserted_at_front(){}

        void insert_timer(Timer::ptr timer);

        // 定时器条件触发函数, 当条件对象还存在时执行回调
        static void on_timer(const std::weak_ptr<void> &weak_cond, const Timer::Callback &cb);

        bool delete_clock_rollback();
    private:
        mutable MutexType mutex_;
        std::set<Timer::ptr, Timer::Comparator> timers_;
        std::atomic<bool> tickled_{false}; // 是否被唤醒

        std::chrono::time_point<std::chrono::system_clock> last_time_;
    };
} // namespace zlynx

#endif //ZLYNX_TIMER_H
