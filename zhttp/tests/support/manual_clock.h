#ifndef ZLYNX_ZHTTP_TESTS_SUPPORT_MANUAL_CLOCK_H_
#define ZLYNX_ZHTTP_TESTS_SUPPORT_MANUAL_CLOCK_H_

#include <chrono>
#include <cstdint>

namespace zhttp {
namespace test {
extern thread_local int64_t manual_nanoseconds;

class ManualClock {
  public:
    ManualClock() : previous_(manual_nanoseconds) {
        manual_nanoseconds = 100000000000LL;
    }

    ~ManualClock() { manual_nanoseconds = previous_; }

    template <class Rep, class Period>
    void advance(std::chrono::duration<Rep, Period> duration) {
        manual_nanoseconds +=
            std::chrono::duration_cast<std::chrono::nanoseconds>(duration)
                .count();
    }

  private:
    int64_t previous_;
};
} // namespace test
} // namespace zhttp

#endif
