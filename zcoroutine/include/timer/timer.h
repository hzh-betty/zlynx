#ifndef ZCOROUTINE_TIMER_H_
#define ZCOROUTINE_TIMER_H_

#include <functional>
#include <memory>

namespace zcoroutine {

class TimerManager;

/**
 * @brief 定时器类
 * 表示单个定时器，支持一次性和循环两种模式
 */
class Timer : public std::enable_shared_from_this<Timer> {
public:
  using ptr = std::shared_ptr<Timer>;
  using Callback = std::function<void()>;

  /**
   * @brief 取消定时器
   * @return 成功返回true，已取消返回false
   */
  bool cancel();

  /**
   * @brief 刷新定时器
   * 重新计算下次触发时间（从现在开始）
   * @return 成功返回true，已取消返回false
   */
  bool refresh();

  /**
   * @brief 重置超时时间
   * @param ms 新的超时时间（毫秒）
   * @param from_now 是否从当前时间开始计算
   * @return 成功返回true，已取消返回false
   */
  bool reset(uint64_t ms, bool from_now = true);

  /**
   * @brief 获取下次触发时间
   * @return 绝对时间（毫秒）
   */
  uint64_t get_next_time() const { return next_time_; }

  /**
   * @brief 是否循环定时器
   */
  bool is_recurring() const { return recurring_; }

  /**
   * @brief 获取回调函数
   */
  const Callback &callback() const { return callback_; }

  /**
   * @brief 私有构造函数，只能通过TimerManager创建
   */
  Timer(uint64_t interval_ms, Callback callback, bool recurring,
        TimerManager *manager);

  /**
   * @brief 用于查找的构造函数
   */
  explicit Timer(uint64_t ms);

  /**
   * @brief 从定时器管理器中移除自己
   * @return 成功返回true
   */
  bool erase_timer();

private:
  uint64_t next_time_;     // 下次触发时间（绝对时间，毫秒）
  uint64_t interval_ms_{}; // 周期间隔（毫秒）
  bool recurring_{};       // 是否循环
  Callback callback_;      // 定时器回调
  TimerManager *manager_ = nullptr; // 所属的定时器管理器

  friend class TimerManager;
};

} // namespace zcoroutine

#endif // ZCOROUTINE_TIMER_H_
