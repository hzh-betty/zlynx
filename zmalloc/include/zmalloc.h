#ifndef ZMALLOC_ZMALLOC_H_
#define ZMALLOC_ZMALLOC_H_

/**
 * @file zmalloc.h
 * @brief zmalloc 对外统一接口
 *
 * 提供高性能的内存分配和释放 API。
 */

#include "common.h"
#include "page_cache.h"
#include "thread_cache.h"

namespace zmalloc {

// 前置声明优化：避免在热路径上重复 TLS 查找
namespace internal {

// 快路径分配：直接从 ThreadCache FreeList 弹出（无锁）
// 返回 nullptr 表示需要走慢路径
ZM_ALWAYS_INLINE void *fast_alloc(ThreadCache *tc, size_t size) {
  const SizeClassLookup &e = SizeClass::lookup(size);
  const size_t index = static_cast<size_t>(e.index);
  // 直接访问 FreeList 头部 - 这是最热的路径
  return tc->try_pop_fast(index);
}

// 快路径释放：直接压入 ThreadCache FreeList（无锁）
// 返回 true 表示成功，false 表示需要走慢路径（链表过长）
ZM_ALWAYS_INLINE bool fast_dealloc(ThreadCache *tc, void *ptr, size_t size) {
  const SizeClassLookup &e = SizeClass::lookup(size);
  const size_t index = static_cast<size_t>(e.index);
  return tc->try_push_fast(ptr, index);
}

} // namespace internal

/**
 * @brief 分配内存
 * @param size 请求字节数
 * @return 内存指针，失败抛出 std::bad_alloc
 */
ZM_ALWAYS_INLINE void *zmalloc(size_t size) {
  if (ZM_UNLIKELY(size == 0)) {
    return nullptr;
  }

  if (ZM_LIKELY(size <= MAX_BYTES)) {
    // 热路径：小对象分配
    ThreadCache *tc = get_thread_cache();
    // 尝试快路径（无锁弹出）
    void *ptr = internal::fast_alloc(tc, size);
    if (ZM_LIKELY(ptr != nullptr)) {
      return ptr;
    }
    // 慢路径：需要从 TransferCache/CentralCache 获取
    return tc->allocate(size);
  }

  // 冷路径：大对象分配
  size_t k_page = (size + PAGE_SIZE - 1) >> PAGE_SHIFT;
  PageCache &pc = PageCache::get_instance();
  pc.page_mtx().lock();
  Span *span = pc.new_span(k_page);
  span->is_use = true;
  span->obj_size = size;
  pc.page_mtx().unlock();
  return reinterpret_cast<void *>(span->page_id << PAGE_SHIFT);
}

/**
 * @brief 释放内存
 * @param ptr 内存指针
 */
ZM_ALWAYS_INLINE void zfree(void *ptr) {
  if (ZM_UNLIKELY(ptr == nullptr)) {
    return;
  }

  PageCache &pc = PageCache::get_instance();
  Span *span = pc.map_object_to_span(ptr);
  const size_t size = span->obj_size;

  if (ZM_LIKELY(size <= MAX_BYTES)) {
    // 热路径：小对象释放
    ThreadCache *tc = get_thread_cache();
    // 尝试快路径（无锁压入，不检查过长）
    if (ZM_LIKELY(internal::fast_dealloc(tc, ptr, size))) {
      return;
    }
    // 慢路径：链表过长，需要回收到 CentralCache
    tc->deallocate(ptr, size);
    return;
  }

  // 冷路径：大对象释放
  pc.page_mtx().lock();
  pc.release_span_to_page_cache(span);
  pc.page_mtx().unlock();
}

} // namespace zmalloc

#endif // ZMALLOC_ZMALLOC_H_
