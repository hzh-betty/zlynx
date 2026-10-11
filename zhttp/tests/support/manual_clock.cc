#include "manual_clock.h"

namespace zhttp {
namespace test {
thread_local int64_t manual_nanoseconds = -1;
}
} // namespace zhttp

std::chrono::steady_clock::time_point
real_steady_now() asm("__real__ZNSt6chrono3_V212steady_clock3nowEv");
std::chrono::steady_clock::time_point
wrapped_steady_now() asm("__wrap__ZNSt6chrono3_V212steady_clock3nowEv");

std::chrono::steady_clock::time_point wrapped_steady_now() {
    const auto value = zhttp::test::manual_nanoseconds;
    if (value < 0)
        return real_steady_now();
    return std::chrono::steady_clock::time_point(
        std::chrono::nanoseconds(value));
}
