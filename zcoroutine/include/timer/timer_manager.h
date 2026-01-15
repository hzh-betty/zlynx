#ifndef ZCOROUTINE_TIMER_MANAGER_H_
#define ZCOROUTINE_TIMER_MANAGER_H_

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "timer.h"

namespace zcoroutine {

/**
 * @brief 定时器管理器
 * 管理所有定时器，使用std::set按时间排序
 */
class TimerManager {
public:
  using ptr = std::shared_ptr<TimerManager>;
  using OnTimerInsertedCallback = std::function<void()>;

  TimerManager() = default;

  /**
   * @brief 析构函数
   * 清理所有定时器
   */
  ~TimerManager();

  /**
   * @brief 添加定时器
   * @param timeout 超时时间（毫秒）
   * @param callback 回调函数
   * @param recurring 是否循环
   * @return 定时器智能指针
   */
  Timer::ptr add_timer(uint64_t timeout, std::function<void()> callback,
                       bool recurring = false);

  /**
   * @brief 添加条件定时器
   * @param timeout 超时时间（毫秒）
   * @param callback 回调函数
   * @param weak_cond 弱引用条件，当条件对象失效时定时器自动取消
   * @param recurring 是否循环
   * @return 定时器智能指针
   */
  Timer::ptr add_condition_timer(uint64_t timeout,
                                 std::function<void()> callback,
                                 std::weak_ptr<void> weak_cond,
                                 bool recurring = false);

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

  /**
   * @brief 是否有定时器
   */
  bool has_timer() const;

  /**
   * @brief 设置定时器插入到最前面时的通知回调
   * @param cb 回调函数，当有新定时器插入到队首时会被调用
   */
  void set_on_timer_inserted_at_front(OnTimerInsertedCallback cb) {
    on_timer_inserted_callback_ = std::move(cb);
  }

private:
  /**
   * @brief 插入定时器到集合中
   */
  void insert_timer(Timer::ptr timer);

  /**
   * @brief 检测系统时钟回播
   * @return 如果检测到回播返回true
   */
  bool detect_clock_rollback();

private:
  // 定时器比较器（按next_time_排序）
  struct TimerComparator {
    bool operator()(const Timer::ptr &lhs, const Timer::ptr &rhs) const {
      if (!lhs && !rhs)
        return false;
      if (!lhs)
        return true;
      if (!rhs)
        return false;
      if (lhs->next_time_ != rhs->next_time_) {
        return lhs->next_time_ < rhs->next_time_;
      }
      return lhs.get() < rhs.get();
    }
  };

  std::atomic<bool> is_ticked_{false}; // 是否已经通知协程调度器
  std::set<Timer::ptr, TimerComparator> timers_; // 定时器集合
  mutable std::mutex mutex_;                     // 互斥锁
  uint64_t last_time_ = 0;                       // 上次检测时间
  OnTimerInsertedCallback on_timer_inserted_callback_; // 定时器插入队首时的回调

  friend class Timer;
};

} // namespace zcoroutine

#endif // ZCOROUTINE_TIMER_MANAGER_H_
