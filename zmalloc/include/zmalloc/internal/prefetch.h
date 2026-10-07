/**
 * @file prefetch.h
 * @brief 内存预取指令封装
 * @author hzh-betty
 *
 * 提供跨平台的内存预取接口，用于优化内存访问性能。
 * 参考 tcmalloc 的 prefetch.h 实现。
 */

#ifndef ZMALLOC_INTERNAL_PREFETCH_H_
#define ZMALLOC_INTERNAL_PREFETCH_H_

namespace zmalloc {

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

#endif // ZMALLOC_INTERNAL_PREFETCH_H_
