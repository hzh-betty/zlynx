#pragma once

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

namespace zmalloc {

// 小于等于 MAX_BYTES 找 ThreadCache 申请，大于则找 PageCache 或系统
static constexpr size_t MAX_BYTES = 256 * 1024;

// ThreadCache 和 CentralCache 自由链表哈希桶数量
static constexpr size_t NFREELISTS = 208;

// PageCache 哈希桶数量
static constexpr size_t NPAGES = 129;

// 页大小偏移，一页 = 2^13 = 8KB
static constexpr size_t PAGE_SHIFT = 13;

// 页号类型
using PageId = uintptr_t;

/**
 * @brief 向系统申请 kpage 页内存
 * @param kpage 页数
 * @return 内存指针，失败抛出 std::bad_alloc
 */
inline void *system_alloc(size_t kpage) {
  void *ptr = mmap(nullptr, kpage << PAGE_SHIFT, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (ptr == MAP_FAILED) {
    throw std::bad_alloc();
  }
  return ptr;
}

/**
 * @brief 释放内存给系统
 * @param ptr 内存指针
 * @param kpage 页数
 */
inline void system_free(void *ptr, size_t kpage) {
  munmap(ptr, kpage << PAGE_SHIFT);
}

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
  /**
   * @brief 头插一个对象
   * @param obj 对象指针
   */
  void push(void *obj) {
    assert(obj);
    next_obj(obj) = free_list_;
    free_list_ = obj;
    ++size_;
  }

  /**
   * @brief 头删一个对象
   * @return 对象指针
   */
  void *pop() {
    assert(free_list_);
    void *obj = free_list_;
    free_list_ = next_obj(free_list_);
    --size_;
    return obj;
  }

  /**
   * @brief 头插一段范围的对象
   * @param start 起始指针
   * @param end 结束指针
   * @param n 对象数量
   */
  void push_range(void *start, void *end, size_t n) {
    assert(start && end);
    next_obj(end) = free_list_;
    free_list_ = start;
    size_ += n;
  }

  /**
   * @brief 头删一段范围的对象
   * @param start 输出起始指针
   * @param end 输出结束指针
   * @param n 对象数量
   */
  void pop_range(void *&start, void *&end, size_t n) {
    assert(n <= size_);
    start = free_list_;
    end = start;
    for (size_t i = 0; i < n - 1; ++i) {
      end = next_obj(end);
    }
    free_list_ = next_obj(end);
    next_obj(end) = nullptr;
    size_ -= n;
  }

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
  /**
   * @brief 向上对齐（位运算实现）
   */
  static size_t round_up(size_t bytes, size_t align_num) {
    return (bytes + align_num - 1) & ~(align_num - 1);
  }

  /**
   * @brief 根据字节数获取对齐后的大小
   */
  static size_t round_up(size_t bytes) {
    if (bytes <= 128) {
      return round_up(bytes, 8);
    } else if (bytes <= 1024) {
      return round_up(bytes, 16);
    } else if (bytes <= 8 * 1024) {
      return round_up(bytes, 128);
    } else if (bytes <= 64 * 1024) {
      return round_up(bytes, 1024);
    } else if (bytes <= 256 * 1024) {
      return round_up(bytes, 8 * 1024);
    } else {
      // 大于 256KB 按页对齐
      return round_up(bytes, 1 << PAGE_SHIFT);
    }
  }

  /**
   * @brief 获取哈希桶索引（位运算实现）
   */
  static size_t index(size_t bytes, size_t align_shift) {
    return ((bytes + (1 << align_shift) - 1) >> align_shift) - 1;
  }

  /**
   * @brief 根据字节数获取哈希桶索引
   */
  static size_t index(size_t bytes) {
    // 每个区间的桶数量
    static constexpr size_t kGroupArray[4] = {16, 56, 56, 56};
    if (bytes <= 128) {
      return index(bytes, 3);
    } else if (bytes <= 1024) {
      return index(bytes - 128, 4) + kGroupArray[0];
    } else if (bytes <= 8 * 1024) {
      return index(bytes - 1024, 7) + kGroupArray[0] + kGroupArray[1];
    } else if (bytes <= 64 * 1024) {
      return index(bytes - 8 * 1024, 10) + kGroupArray[0] + kGroupArray[1] +
             kGroupArray[2];
    } else if (bytes <= 256 * 1024) {
      return index(bytes - 64 * 1024, 13) + kGroupArray[0] + kGroupArray[1] +
             kGroupArray[2] + kGroupArray[3];
    } else {
      assert(false);
      return static_cast<size_t>(-1);
    }
  }

  /**
   * @brief ThreadCache 一次从 CentralCache 获取对象的上限
   */
  static size_t num_move_size(size_t size) {
    assert(size > 0);
    // 对象越小，上限越高；对象越大，上限越低
    size_t num = MAX_BYTES / size;
    if (num < 2)
      num = 2;
    if (num > 512)
      num = 512;
    return num;
  }

  /**
   * @brief CentralCache 一次向 PageCache 获取的页数
   */
  static size_t num_move_page(size_t size) {
    size_t num = num_move_size(size);
    size_t npage = (num * size) >> PAGE_SHIFT;
    if (npage == 0)
      npage = 1;
    return npage;
  }
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

  void push_front(Span *span) { insert(begin(), span); }

  Span *pop_front() {
    Span *front = head_->next;
    erase(front);
    return front;
  }

  void insert(Span *pos, Span *new_span) {
    assert(pos && new_span);
    Span *prev = pos->prev;
    prev->next = new_span;
    new_span->prev = prev;
    new_span->next = pos;
    pos->prev = new_span;
  }

  void erase(Span *pos) {
    assert(pos && pos != head_);
    Span *prev = pos->prev;
    Span *next = pos->next;
    prev->next = next;
    next->prev = prev;
  }

public:
  std::mutex mtx; // 桶锁

private:
  Span *head_;
  static ObjectPool<Span> &span_pool();
};

} // namespace zmalloc
