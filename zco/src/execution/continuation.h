#pragma once
#include "execution/linux/native_context.h"
#include "execution/stack_arena.h"

namespace zco {
namespace detail {
#ifdef ZCO_BENCHMARK_METRICS
uint64_t snapshot_bytes_copied();
#endif
class Continuation {
  public:
    Continuation(StackArena &arena, void (*entry)());
    ~Continuation();
    void resume(NativeContext &caller);
    void suspend(NativeContext &caller);
    void interrupt(NativeContext &caller, std::exception_ptr failure);
    void save(); // Only called on the caller stack, after suspend.
  private:
    StackArena &arena_;
    std::unique_ptr<StackBuffer> independent_;
    StackBuffer *stack_;
    NativeContext context_;
    std::vector<char> snapshot_;
    std::exception_ptr interruption_;
};
} // namespace detail
} // namespace zco
