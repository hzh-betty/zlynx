#ifndef ZCOROUTINE_STACK_ALLOCATOR_H_
#define ZCOROUTINE_STACK_ALLOCATOR_H_

#include <cstddef>
#include <cstdint>

namespace zcoroutine {

/**
 * @brief 栈内存分配器
 * 负责协程栈内存的分配和释放
 * 使用malloc/free实现，支持自定义栈大小
 */
class StackAllocator {
public:
    /**
     * @brief 默认栈大小：128KB
     */
    static constexpr size_t kDefaultStackSize = 128 * 1024;

    /**
     * @brief 分配栈内存
     * @param size 栈大小（字节）
     * @return 栈内存指针，失败返回nullptr
     */
    static void* allocate(size_t size = kDefaultStackSize);

    /**
     * @brief 释放栈内存
     * @param ptr 栈内存指针
     * @param size 栈大小（字节）
     */
    static void deallocate(void* ptr, size_t size);

    /**
     * @brief 获取默认栈大小
     * @return 默认栈大小（字节）
     */
    static size_t get_default_size() { return kDefaultStackSize; }

private:
    StackAllocator() = delete;
    ~StackAllocator() = delete;
};

} // namespace zcoroutine

#endif // ZCOROUTINE_STACK_ALLOCATOR_H_
