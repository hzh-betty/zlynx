#ifndef ZCOROUTINE_TIMER_H_
#define ZCOROUTINE_TIMER_H_

#include <cstdint>
#include <functional>
#include <memory>

namespace zcoroutine {

/**
 * @brief 定时器类
 * 表示单个定时器，支持一次性和循环两种模式
 */
class Timer : public std::enable_shared_from_this<Timer> {
public:
    using ptr = std::shared_ptr<Timer>;

    /**
     * @brief 构造函数
     * @param timeout 超时时间（毫秒）
     * @param callback 定时器回调函数
     * @param recurring 是否循环，默认false
     */
    Timer(uint64_t timeout, std::function<void()> callback, bool recurring = false);

    /**
     * @brief 取消定时器
     * @return 成功返回true，失败返回false
     */
    bool cancel();

    /**
     * @brief 刷新定时器
     * 重新计算下次触发时间
     */
    void refresh();

    /**
     * @brief 重置超时时间
     * @param timeout 新的超时时间（毫秒）
     */
    void reset(uint64_t timeout);

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
     * @brief 执行定时器回调
     */
    void execute();

private:
    uint64_t next_time_;                // 下次触发时间（绝对时间，毫秒）
    uint64_t interval_;                 // 周期间隔（毫秒）
    bool recurring_;                    // 是否循环
    std::function<void()> callback_;    // 定时器回调
    bool cancelled_ = false;            // 是否已取消

    friend class TimerManager;
};

} // namespace zcoroutine

#endif // ZCOROUTINE_TIMER_H_
