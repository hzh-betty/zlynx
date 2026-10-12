#ifndef ZLYNX_ZCO_TESTS_SUPPORT_RUNTIME_FIXTURE_H_
#define ZLYNX_ZCO_TESTS_SUPPORT_RUNTIME_FIXTURE_H_

#include "zco/zco.h"
#include <atomic>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <thread>

namespace zco {
namespace test {
template <class Predicate> bool wait_until(Predicate predicate) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::yield();
    }
    return true;
}

/**
 * @brief 异常或致命测试断言发生时释放阻塞工作线程的门闩，避免清理时死锁。
 * 工作线程按值捕获 shared_future，使共享状态不依赖此对象的存活。
 */
class ReleasePromise : public std::promise<void> {
  public:
    ~ReleasePromise() {
        try {
            set_value();
        } catch (const std::future_error &) {
        }
    }
};

inline Deadline soon() { return Deadline::after(std::chrono::seconds(3)); }

inline Deadline ms(int value) {
    return Deadline::after(std::chrono::milliseconds(value));
}

inline TaskHandle spawn(Runtime &runtime, Task task) {
    return std::move(runtime.spawn(std::move(task))).value();
}

inline TaskHandle spawn(Executor executor, Task task) {
    return std::move(executor.spawn(std::move(task))).value();
}
} // namespace test
} // namespace zco

#endif
