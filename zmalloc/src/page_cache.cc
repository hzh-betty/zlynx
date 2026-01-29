/**
 * @file page_cache.cc
 * @brief PageCache 实现
 */

#include "page_cache.h"

#include "system_alloc.h"

namespace zmalloc {

Span *PageCache::new_span(size_t k) {
  assert(k > 0);

  // 关键策略：
  // - 小于等于 (NPAGES-1) 的 span：在 PageCache 内按页数分桶管理，可切分/合并。
  // - 大于 (NPAGES-1) 的大 span：直接走系统申请与系统释放，避免进入桶管理。
  if (k > NPAGES - 1) {
    void *ptr = system_alloc(k);
    Span *span = span_pool_.allocate();
    span->page_id = reinterpret_cast<PageId>(ptr) >> PAGE_SHIFT;
    span->n = k;
    span->is_use = true;

    // 关键点：大对象只需要“起始页”映射即可。
    // 原因：对大对象，zfree 传回的就是分配时返回的起始地址，不需要中间页映射。
    id_span_map_.set(span->page_id, span);
    return span;
  }

  // 关键步骤：优先从精确桶（k 页桶）直接取，避免切分。
  if (!span_lists_[k].empty()) {
    Span *k_span = span_lists_[k].pop_front();

    // 该 Span 被分配出去，标记为在用。
    k_span->is_use = true;

    // 关键步骤：小对象 span 需要为每一页建立映射，支持：
    // - map_object_to_span（任意对象地址 -> 页号 -> span）
    // - Central/Thread 回收时按对象地址找到 span
    id_span_map_.set_range(k_span->page_id, k_span->n, k_span);
    return k_span;
  }

  // 关键步骤：向上找更大的桶，切分一个 k 页 span。
  // 切分规则：从大 span 的“头部”切出 k 页，剩余部分回挂到对应桶。
  for (size_t i = k + 1; i < NPAGES; ++i) {
    if (!span_lists_[i].empty()) {
      Span *n_span = span_lists_[i].pop_front();
      Span *k_span = span_pool_.allocate();

      // 在 n_span 头部切 k 页
      k_span->page_id = n_span->page_id;
      k_span->n = k;
      k_span->is_use = true;

      n_span->page_id += k;
      n_span->n -= k;
      n_span->is_use = false;

      // 剩余部分挂到对应桶
      span_lists_[n_span->n].push_front(n_span);

      // 关键步骤：为“剩余 span”建立首尾页映射，用于后续合并。
      // 注意：为了节省空间，只在空闲 span 上维护首尾页映射即可完成合并判断。
      id_span_map_.set(n_span->page_id, n_span);
      id_span_map_.set(n_span->page_id + n_span->n - 1, n_span);

      // 建立 k_span 所有页的映射
      id_span_map_.set_range(k_span->page_id, k_span->n, k_span);
      return k_span;
    }
  }

  // 没有可切分的大页 span：向系统申请 (NPAGES-1) 页作为“补货”，挂入最大桶。
  // 然后递归再走一次 new_span(k)（此时一定能在向上搜索中命中）。
  Span *big_span = span_pool_.allocate();
  void *ptr = system_alloc(NPAGES - 1);
  big_span->page_id = reinterpret_cast<PageId>(ptr) >> PAGE_SHIFT;
  big_span->n = NPAGES - 1;
  big_span->is_use = false;

  span_lists_[big_span->n].push_front(big_span);

  // 递归调用
  return new_span(k);
}

void PageCache::release_span_to_page_cache(Span *span) {
  // 大于 128 页直接释放给系统
  if (span->n > NPAGES - 1) {
    void *ptr = reinterpret_cast<void *>(span->page_id << PAGE_SHIFT);
    // 清理起始页映射，避免遗留指向已回收 Span 的条目。
    id_span_map_.set(span->page_id, nullptr);
    system_free(ptr, span->n);
    span_pool_.deallocate(span);
    return;
  }

  // 关键步骤：尝试与相邻空闲 span 合并，减少外碎片。
  // 停止条件：
  // - 相邻 span 不存在
  // - 相邻 span 正在使用（is_use==true）
  // - 合并后超过桶最大管理页数（NPAGES-1）
  // 注意：这里依赖 id_span_map_ 的“边界页映射”来定位相邻 span。
  //
  // 1) 向前合并
  while (true) {
    PageId prev_id = span->page_id - 1;
    Span *ret = static_cast<Span *>(id_span_map_.get(prev_id));
    if (ret == nullptr) {
      break;
    }
    Span *prev_span = ret;
    if (prev_span->is_use) {
      break;
    }
    if (prev_span->n + span->n > NPAGES - 1) {
      break;
    }

    const PageId prev_start = prev_span->page_id;
    const PageId prev_end = prev_span->page_id + prev_span->n - 1;

    span->page_id = prev_start;
    span->n += prev_span->n;

    // prev_span 即将被回收：先更新其边界页映射到新 span。
    // 这是为了避免 map 中遗留悬空指针（会导致后续合并/映射出错）。
    id_span_map_.set(prev_start, span);
    id_span_map_.set(prev_end, span);

    span_lists_[prev_span->n].erase(prev_span);
    span_pool_.deallocate(prev_span);
  }

  // 2) 向后合并
  while (true) {
    PageId next_id = span->page_id + span->n;
    Span *ret = static_cast<Span *>(id_span_map_.get(next_id));
    if (ret == nullptr) {
      break;
    }
    Span *next_span = ret;
    if (next_span->is_use) {
      break;
    }
    if (next_span->n + span->n > NPAGES - 1) {
      break;
    }

    const PageId next_start = next_span->page_id;
    const PageId next_end = next_span->page_id + next_span->n - 1;

    span->n += next_span->n;

    // next_span 即将被回收：同样先更新其边界页映射。
    id_span_map_.set(next_start, span);
    id_span_map_.set(next_end, span);

    span_lists_[next_span->n].erase(next_span);
    span_pool_.deallocate(next_span);
  }

  // 关键步骤：合并完成后，按最终页数把 span 挂回对应桶，并建立首尾页映射。
  span_lists_[span->n].push_front(span);

  // 建立首尾页映射
  id_span_map_.set(span->page_id, span);
  id_span_map_.set(span->page_id + span->n - 1, span);

  span->is_use = false;
}

} // namespace zmalloc
