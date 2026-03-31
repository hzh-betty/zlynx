#ifndef ZCOROUTINE_INTERNAL_CONTEXT_H_
#define ZCOROUTINE_INTERNAL_CONTEXT_H_

#include <ucontext.h>

#include <cstddef>

namespace zcoroutine {

class Context {
 public:
  Context() = default;
  ~Context() = default;

  ucontext_t* get() { return &ctx_; }
  const ucontext_t* get() const { return &ctx_; }

  void make_context(void* stack_ptr, size_t stack_size, void (*func)(), ucontext_t* link = nullptr);

  static int swap_context(Context* from_ctx, Context* to_ctx);

  int get_context();

  void* get_stack_pointer() const;

 private:
  ucontext_t ctx_{};
};

}  // namespace zcoroutine

#endif  // ZCOROUTINE_INTERNAL_CONTEXT_H_