#ifndef ZMALLOC_PREFETCH_H_
#define ZMALLOC_PREFETCH_H_

/**
 * @file prefetch.h
 * @brief 内存预取指令封装
 *
 * 提供跨平台的内存预取接口，用于优化内存访问性能。
 * 参考 tcmalloc 的 prefetch.h 实现。
 */

namespace zmalloc {

/**
 * @brief 预取数据到缓存（高度时间局部性）
 *
 * 对应 Intel PREFETCHT0 指令，数据保留在所有级别的缓存中。
 * 适用于即将多次访问的数据。
 *
 * @param addr 要预取的内存地址
 */
inline void prefetch_t0(const void *addr) {
#if defined(__GNUC__) || defined(__clang__)
  __builtin_prefetch(addr, 0, 3);
#endif
}

/**
 * @brief 预取数据到缓存（中度时间局部性）
 *
 * 对应 Intel PREFETCHT1 指令。
 *
 * @param addr 要预取的内存地址
 */
inline void prefetch_t1(const void *addr) {
#if defined(__GNUC__) || defined(__clang__)
  __builtin_prefetch(addr, 0, 2);
#endif
}

/**
 * @brief 预取数据到缓存（低度时间局部性）
 *
 * 对应 Intel PREFETCHT2 指令。
 *
 * @param addr 要预取的内存地址
 */
inline void prefetch_t2(const void *addr) {
#if defined(__GNUC__) || defined(__clang__)
  __builtin_prefetch(addr, 0, 1);
#endif
}

/**
 * @brief 预取数据（无时间局部性）
 *
 * 对应 Intel PREFETCHNTA 指令，数据读取后不需要保留在缓存中。
 * 适用于流式访问模式。
 *
 * @param addr 要预取的内存地址
 */
inline void prefetch_nta(const void *addr) {
#if defined(__GNUC__) || defined(__clang__)
  __builtin_prefetch(addr, 0, 0);
#endif
}

/**
 * @brief 预取数据用于写入（高度时间局部性）
 *
 * 使用 PREFETCHW 指令（如果可用），预取数据并获取写权限。
 * 可能使其他 CPU 的缓存副本失效。
 *
 * @param addr 要预取的内存地址
 */
inline void prefetch_w(const void *addr) {
#if defined(__GNUC__) || defined(__clang__)
#if defined(__x86_64__) && !defined(__PRFCHW__)
  // x86_64 上手动生成 PREFETCHW 指令
  asm volatile("prefetchw %0" : : "m"(*static_cast<const char *>(addr)));
#else
  __builtin_prefetch(addr, 1, 3);
#endif
#endif
}

/**
 * @brief 预取下一个链表节点
 *
 * 专门用于链表遍历优化。如果 next 不为空，预取其内容。
 *
 * @param next 下一个节点的指针
 */
inline void prefetch_next(const void *next) {
#if defined(__GNUC__) || defined(__clang__)
  if (next != nullptr) {
    __builtin_prefetch(next, 0, 3);
  }
#endif
}

} // namespace zmalloc

#endif // ZMALLOC_PREFETCH_H_
