#ifndef ZLYNX_ZCO_INCLUDE_ZCO_DEADLINE_H_
#define ZLYNX_ZCO_INCLUDE_ZCO_DEADLINE_H_

#include <chrono>
#include <limits>

namespace zco {
/**
 * @brief 基于单调时钟的绝对截止时间，默认值表示无限等待。
 * @note 多轮等待复用同一值，不会因重试重新开始计时。
 */
class Deadline {
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    Deadline() : time_(TimePoint::max()) {}

    static Deadline infinite() { return {}; }

    static Deadline at(TimePoint time) { return Deadline(time); }

    /**
     * @brief 将相对时长转换为当前时刻起的绝对截止时间。
     * @return 非正时长表示立即到期；超出时钟可表示范围时返回无限截止时间。
     */
    template <class Rep, class Period>
    static Deadline after(std::chrono::duration<Rep, Period> duration) {
        auto now = Clock::now();
        if (duration <= duration.zero())
            return at(now);
        auto remaining = TimePoint::max() - now;
        if (std::chrono::duration<long double>(duration) >=
            std::chrono::duration<long double>(remaining))
            return infinite();
        return at(now + std::chrono::duration_cast<Clock::duration>(duration));
    }

    bool is_infinite() const { return time_ == TimePoint::max(); }

    bool expired(TimePoint now) const { return !is_infinite() && now >= time_; }

    TimePoint time() const { return time_; }

  private:
    explicit Deadline(TimePoint time) : time_(time) {}

    TimePoint time_;
};
} // namespace zco

#endif
