#pragma once
#include <cstddef>
#include <ucontext.h>

namespace zco {
namespace detail {
struct NativeContext {
    ucontext_t context{};
    int saved_errno = 0;
};

void make_context(NativeContext &, void *stack, size_t size, void (*entry)());
void switch_context(NativeContext &from, NativeContext &to);
char *stack_pointer(const NativeContext &);
} // namespace detail
} // namespace zco
