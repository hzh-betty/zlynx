/**
 * @file system_alloc.cc
 * @brief 系统内存分配函数实现，使用 mmap + MAP_FIXED_NOREPLACE 保证对齐
 */

#include <cstdint>
#include <new>
#include <sys/mman.h>

#include "system_alloc.h"
#include "zmalloc_config.h"

// MAP_FIXED_NOREPLACE 可能未定义
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

namespace zmalloc {

namespace {

// 线程局部存储：下一个 mmap 地址 hint
thread_local uintptr_t tls_next_addr = 0;

// 生成对齐的随机 hint 地址
uintptr_t random_aligned_hint(size_t alignment) {
  // 用一次 mmap 获得内核选择的随机地址作为种子
  void *tmp =
      mmap(nullptr, PAGE_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (tmp == MAP_FAILED) {
    return 0;
  }
  munmap(tmp, PAGE_SIZE);
  // 对齐到指定边界
  uintptr_t addr = reinterpret_cast<uintptr_t>(tmp);
  return (addr + alignment - 1) & ~(alignment - 1);
}

} // namespace

void *system_alloc(size_t kpage) {
  size_t size = kpage << PAGE_SHIFT;

  // 关键策略：使用线程局部的地址 hint，让同一线程的 mmap 尽量返回连续区间，
  // 降低虚拟地址碎片与 TLB 压力。
  // 如果 hint 未初始化或发生了不对齐（理论上不应发生），重新生成。
  if (!tls_next_addr || (tls_next_addr & (PAGE_SIZE - 1))) {
    tls_next_addr = random_aligned_hint(PAGE_SIZE);
  }

  // 关键步骤：优先尝试 MAP_FIXED_NOREPLACE + hint。
  // - 目的：在“该地址可用”时，强制拿到对齐且可预期的位置；
  // - 如果地址不可用，内核返回失败（或返回非 hint 地址），我们重试。
  // 这里最多重试 100 次，避免在地址空间紧张时长时间自旋。
  for (int i = 0; i < 100; ++i) {
    if (!tls_next_addr) {
      tls_next_addr = random_aligned_hint(PAGE_SIZE);
      if (!tls_next_addr) {
        break; // 无法获取 hint，跳出使用无 hint 分配
      }
    }

    void *hint = reinterpret_cast<void *>(tls_next_addr);
    void *result =
        mmap(hint, size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

    if (result == hint) {
      // 成功：地址已对齐，更新下一次 hint
      tls_next_addr += size;
      return result;
    }

    if (result != MAP_FAILED) {
      // 返回了不同的地址，释放重试
      munmap(result, size);
    }

    // 生成新的 hint 重试
    tls_next_addr = random_aligned_hint(PAGE_SIZE);
  }

  // 降级：不用 hint，让内核自由选择地址（通常也是页对齐）。
  // 若依然出现非对齐（极少见/平台差异），再走“手动对齐”逻辑。
  void *ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (ptr == MAP_FAILED) {
    throw std::bad_alloc();
  }

  // 检查是否对齐：如果不对齐则需要手动调整。
  // 做法：申请 size+PAGE_SIZE 的更大区间，选择其中对齐的子区间，
  // 再把前缀/后缀多余部分 munmap 掉。
  uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
  if (addr & (PAGE_SIZE - 1)) {
    // 不对齐：释放重新申请更大的内存并对齐
    munmap(ptr, size);
    size_t alloc_size = size + PAGE_SIZE;
    ptr = mmap(nullptr, alloc_size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
      throw std::bad_alloc();
    }
    // 计算对齐地址
    addr = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t aligned = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    // 释放前后多余部分
    size_t prefix = aligned - addr;
    size_t suffix = alloc_size - prefix - size;
    if (prefix > 0) {
      munmap(ptr, prefix);
    }
    if (suffix > 0) {
      munmap(reinterpret_cast<void *>(aligned + size), suffix);
    }
    ptr = reinterpret_cast<void *>(aligned);
  }

  return ptr;
}

void system_free(void *ptr, size_t kpage) { munmap(ptr, kpage << PAGE_SHIFT); }

} // namespace zmalloc
