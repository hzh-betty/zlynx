/**
 * @file system_alloc.cc
 * @brief 系统内存分配函数实现，使用 mmap + MAP_FIXED_NOREPLACE 保证对齐
 */

#include "common.h"

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

  // 如果没有初始化或当前地址未对齐，生成新的对齐地址
  if (!tls_next_addr || (tls_next_addr & (PAGE_SIZE - 1))) {
    tls_next_addr = random_aligned_hint(PAGE_SIZE);
  }

  // 尝试使用 hint 分配 (最多重试 100 次)
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

  // 降级：无 hint 分配（可能不对齐，但至少能分配）
  void *ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (ptr == MAP_FAILED) {
    throw std::bad_alloc();
  }

  // 检查是否对齐，如果不对齐则需要手动调整
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
