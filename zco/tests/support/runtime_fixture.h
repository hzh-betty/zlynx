#pragma once
#include "zco/zco.h"
#include <atomic>
#include <future>
#include <gtest/gtest.h>
#include <thread>

namespace zco {
namespace test {
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
