#include "runtime/shared_stack_pool.h"
#include "runtime/fiber.h"
#include "zcoroutine_logger.h"
#include <cstdlib>
#include <cstring>

namespace zcoroutine {

SharedStackPool::SharedStackPool(int count, size_t stack_size)
    : count_(count)
    , stack_size_(stack_size)
    , alloc_idx_(0) {
    
    ZCOROUTINE_LOG_INFO("SharedStackPool creating: count={}, stack_size={}", count, stack_size);

    // 预分配所有共享栈
    stack_array_.reserve(count);
    for (int i = 0; i < count; ++i) {
        StackMem* stack_mem = new StackMem();
        stack_mem->stack_size = stack_size;
        stack_mem->stack_buffer = static_cast<char*>(malloc(stack_size));
        stack_mem->stack_bp = stack_mem->stack_buffer + stack_size;  // 栈顶=栈底+栈大小
        stack_mem->occupy_fiber = nullptr;
        
        if (!stack_mem->stack_buffer) {
            ZCOROUTINE_LOG_FATAL("SharedStackPool malloc failed: index={}, stack_size={}", i, stack_size);
            abort();
        }
        
        stack_array_.push_back(stack_mem);
        ZCOROUTINE_LOG_DEBUG("SharedStackPool allocated stack: index={}, buffer={}, bp={}", 
                             i, static_cast<void*>(stack_mem->stack_buffer), 
                             static_cast<void*>(stack_mem->stack_bp));
    }
    
    ZCOROUTINE_LOG_INFO("SharedStackPool created: count={}, stack_size={}, total_memory={}KB", 
                        count, stack_size, (count * stack_size) / 1024);
}

SharedStackPool::~SharedStackPool() {
    ZCOROUTINE_LOG_INFO("SharedStackPool destroying: count={}", count_);
    
    size_t freed_count = 0;
    for (auto* stack_mem : stack_array_) {
        if (stack_mem) {
            if (stack_mem->stack_buffer) {
                free(stack_mem->stack_buffer);
                freed_count++;
            }
            delete stack_mem;
        }
    }
    stack_array_.clear();
    
    ZCOROUTINE_LOG_INFO("SharedStackPool destroyed: freed {} stacks", freed_count);
}

StackMem* SharedStackPool::allocate_stack() {
    // 轮询分配策略
    unsigned int idx = alloc_idx_.fetch_add(1, std::memory_order_relaxed) % count_;
    StackMem* stack = stack_array_[idx];
    
    ZCOROUTINE_LOG_DEBUG("SharedStackPool::allocate_stack: allocated index={}, buffer={}", 
                         idx, static_cast<void*>(stack->stack_buffer));
    return stack;
}

void SharedStackPool::save_stack(Fiber* fiber) {
    // 此函数需要访问Fiber的私有成员，实现在fiber.cc中
    // 这里只是声明，具体实现需要Fiber配合
    ZCOROUTINE_LOG_DEBUG("SharedStackPool::save_stack called for fiber");
}

void SharedStackPool::restore_stack(Fiber* fiber) {
    // 此函数需要访问Fiber的私有成员，实现在fiber.cc中
    // 这里只是声明，具体实现需要Fiber配合
    ZCOROUTINE_LOG_DEBUG("SharedStackPool::restore_stack called for fiber");
}

} // namespace zcoroutine
