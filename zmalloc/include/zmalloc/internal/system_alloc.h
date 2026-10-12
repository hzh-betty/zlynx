/**
 * @file system_alloc.h
 * @author hzh-betty
 */
#ifndef ZMALLOC_INTERNAL_SYSTEM_ALLOC_H_
#define ZMALLOC_INTERNAL_SYSTEM_ALLOC_H_

#include <cstddef>

namespace zmalloc {

/**
 * @brief 向系统申请 kpage 个页大小的连续内存
 * @param kpage 页数；零值不分配。
 * @return PAGE_SIZE 对齐的内存指针；申请失败或参数溢出时抛出 std::bad_alloc
 */
void *system_alloc(size_t kpage);

/**
 * @brief 不抛异常的系统分配入口，供递归分配和启动阶段使用。
 * @return
 * 零页、尺寸溢出或映射失败时返回 nullptr，否则返回 PAGE_SIZE 对齐的地址。

 */
void *system_alloc_nothrow(size_t kpage) noexcept;

/**
 * @brief 将 system_alloc 返回的连续内存归还给系统
 * @pre ptr 与 kpage 必须描述尚未释放的系统分配范围。
 * @param kpage 页数
 */
void system_free(void *ptr, size_t kpage);

/**
 * @brief 建议回收空闲页的物理内存，保留地址；失败返回 false，不抛异常。

 */
bool system_release(void *ptr, size_t kpage) noexcept;

} // namespace zmalloc

#endif
