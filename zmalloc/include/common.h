#ifndef ZMALLOC_COMMON_H_
#define ZMALLOC_COMMON_H_

/**
 * @file common.h
 * @brief zmalloc 公共定义，包含常量、类型、对齐算法、自由链表和 Span 管理
 */

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <new>
#include <sys/mman.h>

// MAP_FIXED_NOREPLACE 可能未定义（老版本内核）
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace zmalloc {

// 小于等于 MAX_BYTES 找 ThreadCache 申请，大于则找 PageCache 或系统
static constexpr size_t MAX_BYTES = 256 * 1024;

// ThreadCache 和 CentralCache 自由链表哈希桶数量
static constexpr size_t NFREELISTS = 208;

// PageCache 哈希桶数量
static constexpr size_t NPAGES = 129;

// 页大小偏移，一页 = 2^13 = 8KB
static constexpr size_t PAGE_SHIFT = 13;

// 页大小
static constexpr size_t PAGE_SIZE = 1 << PAGE_SHIFT;

// 页号类型
using PageId = uintptr_t;

/**
 * @brief 向系统申请 kpage 页对齐内存
 * @param kpage 页数
 * @return 8KB 对齐的内存指针，失败抛出 std::bad_alloc
 */
void *system_alloc(size_t kpage);

/**
 * @brief 释放内存给系统
 * @param ptr 内存指针
 * @param kpage 页数
 */
void system_free(void *ptr, size_t kpage);

/**
 * @brief 获取自由链表中下一个对象的引用
 * @param ptr 当前对象指针
 * @return 下一个对象指针的引用
 */
inline void *&next_obj(void *ptr) { return *static_cast<void **>(ptr); }

/**
 * @brief 自由链表，管理切分好的小对象
 */
class FreeList {
public:
  void push(void *obj);
  void *pop();
  void push_range(void *start, void *end, size_t n);
  void pop_range(void *&start, void *&end, size_t n);

  bool empty() const { return free_list_ == nullptr; }
  size_t size() const { return size_; }
  size_t &max_size() { return max_size_; }

private:
  void *free_list_ = nullptr; // 自由链表头
  size_t max_size_ = 1;       // 慢启动最大批量
  size_t size_ = 0;           // 当前对象数量
};

/**
 * @brief 大小类，管理对齐和映射关系
 *
 * 对齐策略（控制内碎片在 10% 左右）:
 * [1, 128]          8B 对齐     freelist[0, 16)
 * [129, 1024]       16B 对齐    freelist[16, 72)
 * [1025, 8KB]       128B 对齐   freelist[72, 128)
 * [8KB+1, 64KB]     1KB 对齐    freelist[128, 184)
 * [64KB+1, 256KB]   8KB 对齐    freelist[184, 208)
 */
class SizeClass {
public:
  static size_t round_up(size_t bytes, size_t align_num);
  static size_t round_up(size_t bytes);
  static size_t index(size_t bytes, size_t align_shift);
  static size_t index(size_t bytes);
  static size_t num_move_size(size_t size);
  static size_t num_move_page(size_t size);
};

// 前向声明
template <typename T> class ObjectPool;

/**
 * @brief Span 结构，管理以页为单位的大块内存
 */
struct Span {
  PageId page_id = 0; // 起始页号
  size_t n = 0;       // 页数量

  Span *next = nullptr; // 双链表
  Span *prev = nullptr;

  size_t obj_size = 0;       // 切分后的小对象大小
  size_t use_count = 0;      // 已分配给 ThreadCache 的对象计数
  void *free_list = nullptr; // 切分后的小对象自由链表

  bool is_use = false; // 是否正在被使用
};

/**
 * @brief Span 双向循环链表
 */
class SpanList {
public:
  SpanList();

  Span *begin() { return head_->next; }
  Span *end() { return head_; }
  bool empty() const { return head_ == head_->next; }

  void push_front(Span *span);
  Span *pop_front();
  void insert(Span *pos, Span *new_span);
  void erase(Span *pos);

public:
  std::mutex mtx; // 桶锁

private:
  Span *head_;
  static ObjectPool<Span> &span_pool();
};

} // namespace zmalloc

#endif // ZMALLOC_COMMON_H_
