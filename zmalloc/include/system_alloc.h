#ifndef ZMALLOC_SYSTEM_ALLOC_H_
#define ZMALLOC_SYSTEM_ALLOC_H_

#include <cstddef>

namespace zmalloc {

/**
 * @brief 向系统申请 kpage 页对齐内存
 * @param kpage 页数
 * @return PAGE_SIZE 对齐的内存指针，失败抛出 std::bad_alloc
 */
void *system_alloc(size_t kpage);

/**
 * @brief 释放内存给系统
 * @param ptr 内存指针
 * @param kpage 页数
 */
void system_free(void *ptr, size_t kpage);

} // namespace zmalloc

#endif // ZMALLOC_SYSTEM_ALLOC_H_
