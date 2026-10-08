/**
 * @file system_alloc.h
 * @brief system_alloc 定义。
 * @author hzh-betty

 */

#ifndef ZMALLOC_INTERNAL_SYSTEM_ALLOC_H_
#define ZMALLOC_INTERNAL_SYSTEM_ALLOC_H_

#include <cstddef>

namespace zmalloc {

/**
 * @brief 向系统申请 kpage 个页大小的连续内存
 * @param kpage
 * 页数
 * @return PAGE_SIZE 对齐的内存指针；申请失败或参数溢出时抛出 std::bad_alloc

 */
void *system_alloc(size_t kpage);

/** @brief 不抛异常的系统分配入口，供递归分配和 bootstrap 使用。 */
void *system_alloc_nothrow(size_t kpage) noexcept;

/**
 * @brief 将 system_alloc 返回的连续内存归还给系统
 * @param ptr
 * 内存指针
 * @param kpage 页数
 */
void system_free(void *ptr, size_t kpage);

} // namespace zmalloc

#endif // ZMALLOC_INTERNAL_SYSTEM_ALLOC_H_
