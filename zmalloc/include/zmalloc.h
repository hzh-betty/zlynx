/**
 * @file zmalloc.h
 * @brief zmalloc 内存池主接口
 *
 * 提供类似 malloc/free 的简洁 API。
 */

#ifndef ZMALLOC_ZMALLOC_H_
#define ZMALLOC_ZMALLOC_H_

#include <cstddef>

namespace zmalloc {

/**
 * @brief 分配内存
 * @param size 请求大小（字节）
 * @return 分配的内存指针，失败返回 nullptr
 */
void *Allocate(size_t size);

/**
 * @brief 释放内存
 * @param ptr 要释放的内存指针
 */
void Deallocate(void *ptr);

/**
 * @brief 重新分配内存
 * @param ptr 原内存指针
 * @param new_size 新大小
 * @return 新内存指针，失败返回 nullptr
 *
 * 如果 ptr 为 nullptr，等同于 Allocate(new_size)。
 * 如果 new_size 为 0，等同于 Deallocate(ptr) 并返回 nullptr。
 */
void *Reallocate(void *ptr, size_t new_size);

/**
 * @brief 分配对齐的内存
 * @param size 请求大小
 * @param alignment 对齐要求（必须是 2 的幂）
 * @return 对齐的内存指针，失败返回 nullptr
 */
void *AllocateAligned(size_t size, size_t alignment);

/**
 * @brief 分配并清零内存
 * @param num 元素数量
 * @param size 每个元素的大小
 * @return 清零后的内存指针，失败返回 nullptr
 */
void *AllocateZero(size_t num, size_t size);

/**
 * @brief 获取已分配内存的实际大小
 * @param ptr 内存指针
 * @return 实际分配的大小
 */
size_t GetAllocatedSize(void *ptr);

} // namespace zmalloc

#endif // ZMALLOC_ZMALLOC_H_
