#include "runtime/context.h"
#include "zcoroutine_logger.h"
#include <cstring>

namespace zcoroutine {

void Context::make_context(void* stack_ptr, size_t stack_size, void (*func)()) {
    // 获取当前上下文
    getcontext(&ctx_);
    
    // 设置栈信息
    ctx_.uc_stack.ss_sp = stack_ptr;
    ctx_.uc_stack.ss_size = stack_size;
    ctx_.uc_link = nullptr;  // 协程结束后不自动切换
    
    // 创建上下文，关联执行函数
    makecontext(&ctx_, func, 0);
    
    ZCOROUTINE_LOG_DEBUG("Context::make_context stack_ptr={}, stack_size={}", 
                         stack_ptr, stack_size);
}

int Context::swap_context(Context* from_ctx, Context* to_ctx) {
    if (!from_ctx || !to_ctx) {
        ZCOROUTINE_LOG_ERROR("Context::swap_context invalid parameters: from_ctx={}, to_ctx={}", 
                             static_cast<void*>(from_ctx), static_cast<void*>(to_ctx));
        return -1;
    }
    return swapcontext(&from_ctx->ctx_, &to_ctx->ctx_);
}

int Context::get_context() {
    return getcontext(&ctx_);
}

} // namespace zcoroutine
