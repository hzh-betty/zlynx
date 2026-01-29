#ifndef ZMALLOC_CPU_CACHE_H_
#define ZMALLOC_CPU_CACHE_H_

/**
 * @file cpu_cache.h
 * @brief Per-CPU 缓存，减少多线程锁竞争
 *
 * 简化版 Per-CPU Cache 实现：
 * - 使用 sched_getcpu() 获取当前 CPU ID
 * - 每个 CPU 有独立的溢出缓存
 * - 当 ThreadCache 满时，可以快速将对象暂存到 CPU 缓存
 * - 当 ThreadCache 空时，可以从 CPU 缓存快速获取对象
 *
 * 注意：这不是完全无锁的实现（需要 RSEQ），但通过减少跨 CPU 竞争
 * 可以显著提升多线程性能。
 */

#include <atomic>
#include <cstddef>
#include <cstdint>

#ifdef __linux__
#include <sched.h>
#include <unistd.h>
#endif

#include "common.h"
#include "prefetch.h"
#include "zmalloc_config.h"

namespace zmalloc {

// CPU 数量上限（实际使用时会检测系统 CPU 数）
static constexpr size_t kMaxCpus = 256;

// 每个 size class 在 CPU 缓存中的最大对象数
static constexpr size_t kCpuCacheCapacity = 32;

// 注意：CpuCache 内部是 [CPU][SizeClass] 的二维数组：
// slots_[kMaxCpus][NFREELISTS]。
// 这会带来较大的静态内存占用（每个 slot 还包含一个小数组 + 原子计数 + 锁）。
// 当前实现更偏向“实验/性能验证”用途；若要用于生产，可考虑：
// - 降低 kMaxCpus 上限
// - 只为实际 num_cpus_ 动态分配
// - 或按热点 size class 稀疏存储

/**
 * @brief 获取当前 CPU ID
 * @return CPU ID，如果不支持则返回 0
 */
inline int get_current_cpu() {
#ifdef __linux__
  int cpu = sched_getcpu();
  return cpu >= 0 ? cpu : 0;
#else
  return 0;
#endif
}

/**
 * @brief 单个 size class 的 CPU 缓存槽
 *
 * 使用简单的数组存储对象指针，避免链表遍历。
 */
struct alignas(64) CpuCacheSlot {
  void *objects[kCpuCacheCapacity]; // 对象数组
  std::atomic<uint32_t> size{0};    // 当前对象数量
  SpinLock lock;                    // 保护并发访问

  /**
   * @brief 尝试从 CPU 缓存弹出对象
   * @param batch 输出数组
   * @param max_count 最大获取数量
   * @return 实际获取的数量
   */
  size_t try_pop(void **batch, size_t max_count) {
    // 快速检查（无锁）
    // 关键点：这里使用 relaxed 仅用于“快判断”，正确性由锁保护。
    if (size.load(std::memory_order_relaxed) == 0) {
      return 0;
    }

    lock.lock();
    uint32_t current_size = size.load(std::memory_order_relaxed);
    if (current_size == 0) {
      lock.unlock();
      return 0;
    }

    size_t count = (max_count < current_size) ? max_count : current_size;
    uint32_t new_size = current_size - static_cast<uint32_t>(count);

    // 从数组尾部弹出
    for (size_t i = 0; i < count; ++i) {
      batch[i] = objects[new_size + i];
      // 预取下一个要返回的对象
      if (i + 1 < count) {
        prefetch_t0(objects[new_size + i + 1]);
      }
    }

    size.store(new_size, std::memory_order_relaxed);
    lock.unlock();
    return count;
  }

  /**
   * @brief 尝试向 CPU 缓存压入对象
   * @param batch 输入数组
   * @param count 对象数量
   * @return 实际存入的数量
   */
  size_t try_push(void **batch, size_t count) {
    // 快速检查（无锁）
    // 关键点：这里的满判断只是“尽量避免加锁”，并不保证精确；
    // 进入锁后会再次根据 current_size 精确计算可用容量。
    if (size.load(std::memory_order_relaxed) >= kCpuCacheCapacity) {
      return 0;
    }

    lock.lock();
    uint32_t current_size = size.load(std::memory_order_relaxed);
    size_t available = kCpuCacheCapacity - current_size;
    if (available == 0) {
      lock.unlock();
      return 0;
    }

    size_t actual_count = (count < available) ? count : available;

    // 压入数组尾部
    for (size_t i = 0; i < actual_count; ++i) {
      objects[current_size + i] = batch[i];
    }

    size.store(current_size + static_cast<uint32_t>(actual_count),
               std::memory_order_relaxed);
    lock.unlock();
    return actual_count;
  }
};

/**
 * @brief Per-CPU 缓存
 *
 * 每个 CPU 有一组按 size class 索引的缓存槽。
 */
class CpuCache : public NonCopyable {
public:
  static CpuCache &get_instance() {
    static CpuCache instance;
    return instance;
  }

  /**
   * @brief 尝试从当前 CPU 的缓存获取对象
   * @param index size class 索引
   * @param batch 输出数组
   * @param max_count 最大获取数量
   * @return 实际获取的数量
   */
  size_t try_pop(size_t index, void **batch, size_t max_count) {
    int cpu = get_current_cpu();
    if (ZM_UNLIKELY(static_cast<size_t>(cpu) >= num_cpus_)) {
      cpu = 0;
    }
    return slots_[cpu][index].try_pop(batch, max_count);
  }

  /**
   * @brief 尝试向当前 CPU 的缓存存入对象
   * @param index size class 索引
   * @param batch 输入数组
   * @param count 对象数量
   * @return 实际存入的数量
   */
  size_t try_push(size_t index, void **batch, size_t count) {
    int cpu = get_current_cpu();
    if (ZM_UNLIKELY(static_cast<size_t>(cpu) >= num_cpus_)) {
      cpu = 0;
    }
    return slots_[cpu][index].try_push(batch, count);
  }

  /**
   * @brief 获取 CPU 数量
   */
  size_t num_cpus() const { return num_cpus_; }

private:
  CpuCache() : num_cpus_(detect_num_cpus()) {
    // 确保不超过最大值
    if (num_cpus_ > kMaxCpus) {
      num_cpus_ = kMaxCpus;
    }
    if (num_cpus_ == 0) {
      num_cpus_ = 1;
    }
  }

  static size_t detect_num_cpus() {
#ifdef __linux__
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? static_cast<size_t>(n) : 1;
#else
    return 1;
#endif
  }

private:
  size_t num_cpus_;
  // [CPU][SizeClass] 的二维数组
  CpuCacheSlot slots_[kMaxCpus][NFREELISTS];
};

} // namespace zmalloc

#endif // ZMALLOC_CPU_CACHE_H_
