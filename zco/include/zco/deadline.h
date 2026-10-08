#pragma once
#include <chrono>
#include <limits>

namespace zco {
class Deadline {
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    Deadline() : time_(TimePoint::max()) {}

    static Deadline infinite() { return {}; }

    static Deadline at(TimePoint time) { return Deadline(time); }

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
