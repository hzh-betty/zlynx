#include "runtime/stack_allocator.h"
#include "zcoroutine_logger.h"
#include <cstdlib>
#include <cstring>

namespace zcoroutine {

void* StackAllocator::allocate(size_t size) {
    if (size == 0) {
        ZCOROUTINE_LOG_ERROR("StackAllocator::allocate failed: size is 0");
        return nullptr;
    }

    // 使用malloc分配栈内存
    void* ptr = malloc(size);
    if (!ptr) {
        ZCOROUTINE_LOG_ERROR("StackAllocator::allocate malloc failed: requested_size={}", size);
        return nullptr;
    }

    // 清零内存
    memset(ptr, 0, size);
    
    ZCOROUTINE_LOG_DEBUG("StackAllocator::allocate success: ptr={}, size={}", ptr, size);
    return ptr;
}

void StackAllocator::deallocate(void* ptr, size_t size) {
    if (!ptr) {
        ZCOROUTINE_LOG_WARN("StackAllocator::deallocate received null pointer");
        return;
    }

    ZCOROUTINE_LOG_DEBUG("StackAllocator::deallocate: ptr={}, size={}", ptr, size);
    free(ptr);
}

} // namespace zcoroutine
