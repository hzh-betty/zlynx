#include "runtime/stack_allocator.h"

#include <cstddef>
#include <cstdlib>

#include "util/zcoroutine_logger.h"
namespace zcoroutine {

constexpr size_t StackAllocator::kDefaultStackSize;

void *StackAllocator::allocate(size_t size) {
  if (size == 0) {
    ZCOROUTINE_LOG_ERROR("StackAllocator::allocate failed: size is 0");
    return nullptr;
  }

  // 使用malloc分配栈内存
  // 注意：不进行memset清零，因为：
  // 1. 协程栈在使用时会被正常初始化
  // 2. 避免触发大量page fault（memset会touch每个页面）
  // 3. 依赖内核的COW机制，按需分配物理页
  void *ptr = malloc(size);
  if (!ptr) {
    ZCOROUTINE_LOG_ERROR(
        "StackAllocator::allocate malloc failed: requested_size={}", size);
    return nullptr;
  }

  ZCOROUTINE_LOG_DEBUG("StackAllocator::allocate success: ptr={}, size={}", ptr,
                       size);
  return ptr;
}

void StackAllocator::deallocate(void *ptr, size_t size) {
  if (!ptr) {
    ZCOROUTINE_LOG_WARN("StackAllocator::deallocate received null pointer");
    return;
  }

  ZCOROUTINE_LOG_DEBUG("StackAllocator::deallocate: ptr={}, size={}", ptr,
                       size);
  free(ptr);
}

} // namespace zcoroutine
