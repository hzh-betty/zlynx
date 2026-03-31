#include "zcoroutine/internal/context.h"

namespace zcoroutine {

void Context::make_context(void* stack_ptr, size_t stack_size, void (*func)(), ucontext_t* link) {
  getcontext(&ctx_);
  ctx_.uc_stack.ss_sp = stack_ptr;
  ctx_.uc_stack.ss_size = stack_size;
  ctx_.uc_link = link;
  makecontext(&ctx_, func, 0);
}

int Context::swap_context(Context* from_ctx, Context* to_ctx) {
  if (!from_ctx || !to_ctx) {
    return -1;
  }

  return swapcontext(from_ctx->get(), to_ctx->get());
}

int Context::get_context() { return getcontext(&ctx_); }

void* Context::get_stack_pointer() const {
#if defined(__x86_64__)
  return reinterpret_cast<void*>(ctx_.uc_mcontext.gregs[REG_RSP]);
#elif defined(__aarch64__)
  return reinterpret_cast<void*>(ctx_.uc_mcontext.sp);
#else
  return nullptr;
#endif
}

}  // namespace zcoroutine