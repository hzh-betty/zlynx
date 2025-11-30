#include "timer.h"
#include "zlynx_logger.h"

namespace zlynx
{
    bool Timer::cancel()
    {
        ZLYNX_LOG_DEBUG("Timer::cancel called");
        TimerManager::MutexType::Lock lock(manager_->mutex_);
        if (!callback_)
        {
            ZLYNX_LOG_WARN("Timer::cancel failed, already canceled");
            return false;
        }
        callback_ = nullptr;

        if (!erase_timer()) return false;

        ZLYNX_LOG_DEBUG("Timer::cancel succeeded");
        return true;
    }

    bool Timer::refresh()
    {
        ZLYNX_LOG_DEBUG("Timer::refresh called");
        TimerManager::MutexType::Lock lock(manager_->mutex_);
        if (!callback_)
        {
            ZLYNX_LOG_WARN("Timer ::refresh failed, already canceled");
            return false;
        }

        // 找到当前时间点，删除
        if (!erase_timer()) return false;

        // 重新计算下次触发时间
        next_ms_ = std::chrono::system_clock::now() + std::chrono::milliseconds(internal_ms_);
        manager_->timers_.insert(shared_from_this());
        ZLYNX_LOG_DEBUG("Timer::refresh succeeded");
        return true;
    }

    bool Timer::erase_timer()
    {
        const auto iter = manager_->timers_.find(shared_from_this());
        if (iter == manager_->timers_.end())
        {
            ZLYNX_LOG_WARN("Timer::erase_timer()  failed, timer not found in manager");
            return false;
        }
        manager_->timers_.erase(iter);
        return true;
    }

    bool Timer::reset(const uint64_t ms, const bool from_now)
    {
        TimerManager::MutexType::Lock lock(manager_->mutex_);
        ZLYNX_LOG_DEBUG("Timer ::reset called, ms={}, from_now={}", ms, from_now);
        if (!callback_)
        {
            ZLYNX_LOG_DEBUG("Timer ::reset failed, already canceled");
            return false;
        }

        // 找到当前时间点，删除
        if (!erase_timer()) return false;

        // 重新计算下次触发时间
        const auto now = std::chrono::system_clock::now();
        next_ms_ = from_now
                       ? now + std::chrono::milliseconds(ms)
                       : next_ms_ - std::chrono::milliseconds(internal_ms_) + std::chrono::milliseconds(ms);

        internal_ms_ = ms;
        manager_->insert_timer(shared_from_this());
        ZLYNX_LOG_DEBUG("Timer::reset succeeded");
        return true;
    }

    Timer::Timer(const uint64_t internal_ms, Callback callback, const bool recurring, TimerManager *manager)
        : internal_ms_(internal_ms),
          recurring_(recurring),
          callback_(std::move(callback)),
          manager_(manager)
    {
        const auto now = std::chrono::system_clock::now();
        next_ms_ = now + std::chrono::milliseconds(internal_ms_);
    }

    Timer::Timer(uint64_t internal_ms)
    {
        next_ms_ = std::chrono::system_clock::now() + std::chrono::milliseconds(internal_ms_);
    }


    TimerManager::~TimerManager()
    {
        MutexType::Lock lock(mutex_);
        while (!timers_.empty())
        {
            (*timers_.begin())->callback_();
            (*timers_.begin())->callback_ = nullptr;
            timers_.erase(timers_.begin());
        }
        ZLYNX_LOG_DEBUG("TimerManager destroyed");
    }

    Timer::ptr TimerManager::add_timer(const uint64_t ms, const Timer::Callback &cb, const bool recurring)
    {
        ZLYNX_LOG_DEBUG("TimerManager::add_timer called");
        Timer::ptr timer(new Timer(ms, cb, recurring, this));
        insert_timer(timer);
        ZLYNX_LOG_DEBUG("TimerManager::add_timer succeeded");
        return timer;
    }

    Timer::ptr TimerManager::add_condition_timer(const uint64_t ms, const Timer::Callback &cb,
                                                 const std::weak_ptr<void> &weak_cond, const bool recurring)
    {
        ZLYNX_LOG_DEBUG("TimerManager::add_condition_timer called");
        Timer::ptr timer(new Timer(ms,
                                   std::bind(&TimerManager::on_timer, weak_cond, cb),
                                   recurring,
                                   this)); // 绑定条件检查
        insert_timer(timer);
        ZLYNX_LOG_DEBUG("TimerManager::add_condition_timer succeeded");
        return timer;
    }

    uint64_t TimerManager::get_next_expire_time()
    {
        MutexType::Lock lock(mutex_);
        ZLYNX_LOG_DEBUG("TimerManager::get_next_expire_time called");
        tickled_ = false;

        // 如果没有定时器，返回最大值
        if (timers_.empty())
        {
            ZLYNX_LOG_DEBUG("No timers available");
            return ~0ull; // 表示没有定时器
        }

        // 如果下一个定时器已经到期，返回0
        const auto now = std::chrono::system_clock::now();
        const auto next_timer = *timers_.begin();
        if (next_timer->next_ms_ <= now)
        {
            return 0; // 已经过期
        }
        ZLYNX_LOG_DEBUG("Next timer expires in {} ms",
                        std::chrono::duration_cast<std::chrono::milliseconds>(next_timer->next_ms_ - now).count());

        // 返回距离下一个定时器到期的时间
        return std::chrono::duration_cast<std::chrono::milliseconds>(next_timer->next_ms_ - now).count();
    }

    void TimerManager::list_expired_callbacks(std::vector<Timer::Callback> &cbs)
    {
        ZLYNX_LOG_DEBUG("TimerManager::list_expired_callbacks called");
        const auto now = std::chrono::system_clock::now();

        MutexType::Lock lock(mutex_);
        const bool rollback = delete_clock_rollback();

        // 收集到期的定时器回调函数(包括回滚的情况)
        while (!timers_.empty() && (rollback || (*timers_.begin())->next_ms_ <= now))
        {
            auto timer = *timers_.begin();
            timers_.erase(timers_.begin());
            cbs.push_back(timer->callback_);

            // 如果是循环定时器，重新计算下次触发时间并插入集合
            if (timer->recurring_)
            {
                timer->next_ms_ = now + std::chrono::milliseconds(timer->internal_ms_);
                timers_.insert(timer);
            }
            else
            {
                timer->callback_ = nullptr;
            }
        }

        ZLYNX_LOG_DEBUG("TimerManager::list_expired_callbacks completed");
    }

    bool TimerManager::has_timer() const
    {
        MutexType::Lock lock(mutex_);
        return !timers_.empty();
    }


    void TimerManager::insert_timer(Timer::ptr timer)
    {
        bool at_front = false;

        // 插入定时器到集合中
        {
            MutexType::Lock lock(mutex_);
            at_front = timers_.empty() || (timer->next_ms_ < (*timers_.begin())->next_ms_);
            ZLYNX_LOG_DEBUG("Inserting timer, at_front={}", at_front);
            timers_.insert(std::move(timer));
        }

        // 如果插入的定时器在最前面，通知IoManager
        if (at_front && !tickled_)
        {
            tickled_ = true;
            on_timer_inserted_at_front(); // 通知IoManager
            ZLYNX_LOG_DEBUG("Timer inserted at front, notified IoManager");
        }
        ZLYNX_LOG_DEBUG("TimerManager::insert_timer succeeded");
    }

    void TimerManager::on_timer(const std::weak_ptr<void> &weak_cond, const Timer::Callback &cb)
    {
        if (const auto ptr = weak_cond.lock())
        {
            cb();
        }
    }

    bool TimerManager::delete_clock_rollback()
    {
        bool rollback = false;
        const auto now = std::chrono::system_clock::now();
        if (now < (last_time_ - std::chrono::milliseconds(60 * 60 * 1000)))
        {
            rollback = true;
        }
        last_time_ = now;
        return rollback;
    }
} // namespace zlynx
