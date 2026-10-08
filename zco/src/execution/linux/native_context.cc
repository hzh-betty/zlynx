#include "execution/linux/native_context.h"
#include <cerrno>
#include <system_error>
#if !defined(__x86_64__) && !defined(__aarch64__)
#error "zco native contexts support Linux x86_64 and aarch64"
#endif
namespace zco {
namespace detail {
void make_context(NativeContext &ctx, void *stack, size_t size,
                  void (*entry)()) {
    if (::getcontext(&ctx.context) != 0)
        throw std::system_error(errno, std::generic_category(), "getcontext");
    ctx.context.uc_stack.ss_sp = stack;
    ctx.context.uc_stack.ss_size = size;
    ctx.context.uc_link = nullptr;
    ::makecontext(&ctx.context, entry, 0);
}

void switch_context(NativeContext &from, NativeContext &to) {
    from.saved_errno = errno;
    errno = to.saved_errno;
    if (::swapcontext(&from.context, &to.context) != 0)
        throw std::system_error(errno, std::generic_category(), "swapcontext");
    errno = from.saved_errno;
}

char *stack_pointer(const NativeContext &ctx) {
#if defined(__x86_64__)
    return reinterpret_cast<char *>(ctx.context.uc_mcontext.gregs[REG_RSP]);
#else
    return reinterpret_cast<char *>(ctx.context.uc_mcontext.sp);
#endif
}
} // namespace detail
} // namespace zco
