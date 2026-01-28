#ifndef ZMALLOC_COMMON_H_
#define ZMALLOC_COMMON_H_

/**
 * @file common.h
 * @brief zmalloc 公共定义，包含常量、类型、对齐算法、自由链表和 Span 管理
 */

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <thread>

// MAP_FIXED_NOREPLACE 可能未定义
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace zmalloc {

// 分支预测提示
#if defined(__GNUC__) || defined(__clang__)
#define ZM_LIKELY(x) (__builtin_expect(!!(x), 1))
#define ZM_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#define ZM_ALWAYS_INLINE __attribute__((always_inline)) inline
#define ZM_NOINLINE __attribute__((noinline))
#else
#define ZM_LIKELY(x) (x)
#define ZM_UNLIKELY(x) (x)
#define ZM_ALWAYS_INLINE inline
#define ZM_NOINLINE
#endif

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
 * @brief 高性能自旋锁（Linux/C++11）
 *
 * 设计要点（参考 tcmalloc/folly 常见实现思路）：
 * - 使用 load(relaxed) 只读自旋，减少 cache line 抖动
 * - 使用 exchange(acquire) 抢锁，建立同步
 * - unlock 使用 release，形成 happens-before
 * - 指数退避 + 适时 yield，兼顾低竞争与高竞争场景
 */
class alignas(64) SpinLock {
public:
  SpinLock() noexcept = default;
  SpinLock(const SpinLock &) = delete;
  SpinLock &operator=(const SpinLock &) = delete;

  void lock() noexcept {
    // 快速路径：立即尝试获取锁
    if (!locked_.exchange(true, std::memory_order_acquire)) {
      return;
    }
    // 慢路径：指数退避自旋
    lock_slow();
  }

  /**
   * @brief 尝试获取锁（非阻塞）
   * @return true: 成功获取锁; false: 锁被占用
   */
  bool try_lock() noexcept {
    return !locked_.exchange(true, std::memory_order_acquire);
  }

  void unlock() noexcept { locked_.store(false, std::memory_order_release); }

private:
  static constexpr int kMaxSpinCount = 64;
  std::atomic<bool> locked_{false};

  static inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#else
    std::this_thread::yield();
#endif
  }

  void lock_slow() noexcept {
    int spin_count = 1;
    for (;;) {
      for (int i = 0; i < spin_count; ++i) {
        if (!locked_.load(std::memory_order_relaxed)) {
          if (!locked_.exchange(true, std::memory_order_acquire)) {
            return;
          }
        }
        cpu_relax();
      }

      if (spin_count < kMaxSpinCount) {
        spin_count <<= 1;
      } else {
        std::this_thread::yield();
      }
    }
  }
};

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
 *
 */
class FreeList {
public:
  ZM_ALWAYS_INLINE void push(void *obj) {
    assert(obj);
    next_obj(obj) = free_list_;
    free_list_ = obj;
    ++size_;
  }

  ZM_ALWAYS_INLINE void *pop() {
    assert(free_list_);
    void *obj = free_list_;
    free_list_ = next_obj(free_list_);
    --size_;
    return obj;
  }

  void push_range(void *start, void *end, size_t n) {
    assert(start && end);
    next_obj(end) = free_list_;
    free_list_ = start;
    size_ += n;
  }

  void pop_range(void *&start, void *&end, size_t n) {
    // 允许 n==0：不弹出任何元素。
    if (n == 0) {
      start = nullptr;
      end = nullptr;
      return;
    }
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

  size_t pop_batch(void **batch, size_t n) {
    assert(batch);
    if (n == 0) {
      return 0;
    }
    assert(n <= size_);
    void *cur = free_list_;
    for (size_t i = 0; i < n; ++i) {
      batch[i] = cur;
      void *next = next_obj(cur);
      // 简单预取：直接预取下一个对象的内存
      if (next != nullptr) {
        __builtin_prefetch(next, 0, 3);
      }
      cur = next;
    }
    free_list_ = cur;
    next_obj(batch[n - 1]) = nullptr;
    size_ -= n;
    return n;
  }

  ZM_ALWAYS_INLINE bool empty() const { return free_list_ == nullptr; }
  ZM_ALWAYS_INLINE size_t size() const { return size_; }
  ZM_ALWAYS_INLINE size_t &max_size() { return max_size_; }

private:
  // 热数据：每次 push/pop 都访问
  void *free_list_ = nullptr; // 自由链表头
  size_t size_ = 0;           // 当前对象数量
  // 冷数据：只在回收时检查
  size_t max_size_ = 1; // 慢启动最大批量
};

static constexpr size_t kSizeClassLookupLen = (MAX_BYTES / 8) + 1;

struct SizeClassLookup {
  uint32_t align_size;
  uint16_t index;
  uint16_t num_move;
  uint16_t num_pages;
};

extern SizeClassLookup g_size_class_lookup[kSizeClassLookupLen];

// size class index -> align_size 反向映射表（用于 zfree 路径优化）
extern uint32_t g_class_to_size[NFREELISTS];

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

  // 热路径内联辅助：
  // - 小对象按 8 字节分桶查表，避免重复计算对齐/索引/批量策略。
  // - 查表仅覆盖 [1, MAX_BYTES]。
  static inline const SizeClassLookup &lookup(size_t bytes);

  static ZM_ALWAYS_INLINE size_t round_up_fast(size_t bytes) {
    return static_cast<size_t>(lookup(bytes).align_size);
  }

  static ZM_ALWAYS_INLINE size_t index_fast(size_t bytes) {
    return static_cast<size_t>(lookup(bytes).index);
  }

  static ZM_ALWAYS_INLINE void classify(size_t bytes, size_t &align_size,
                                        size_t &index) {
    const SizeClassLookup &e = lookup(bytes);
    align_size = static_cast<size_t>(e.align_size);
    index = static_cast<size_t>(e.index);
  }

  /**
   * @brief 从 size class index 获取对齐后的 size
   * @param index size class 索引
   * @return 对齐后的对象大小
   */
  static ZM_ALWAYS_INLINE size_t class_to_size(size_t index) {
    assert(index < NFREELISTS);
    return static_cast<size_t>(g_class_to_size[index]);
  }
};

ZM_ALWAYS_INLINE const SizeClassLookup &SizeClass::lookup(size_t bytes) {
  if (ZM_UNLIKELY(bytes == 0)) {
    return g_size_class_lookup[0];
  }
  assert(bytes <= MAX_BYTES);
  // 按 8 字节分桶：bucket = ceil(bytes / 8)
  const size_t bucket = (bytes + 7) >> 3;
  return g_size_class_lookup[bucket];
}

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

private:
  Span *head_;
  static ObjectPool<Span> &span_pool();
};

} // namespace zmalloc

#endif // ZMALLOC_COMMON_H_
