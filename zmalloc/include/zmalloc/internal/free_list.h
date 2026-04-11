#ifndef ZMALLOC_INTERNAL_FREE_LIST_H_
#define ZMALLOC_INTERNAL_FREE_LIST_H_

#include <cstddef>

namespace zmalloc {

/**
 * @brief 获取自由链表中下一个对象的引用
 * @param ptr 当前对象指针
 * @return 下一个对象指针的引用
 *
 * FreeList 采用 intrusive 单链表：next 指针直接复用对象首地址存储。
 * 这样可避免额外节点分配，代价是对象最小尺寸必须容纳一个指针。
 */
inline void *&next_obj(void *ptr) { return *static_cast<void **>(ptr); }

/**
 * @brief 小对象自由链表
 *
 * 该结构是 thread cache / central cache 的基础容器，
 * 维护对象链、当前长度与动态批量阈值（max_size_）。
 */
class FreeList {
  public:
    void push(void *obj);
    void *pop();

    void push_range(void *start, void *end, size_t n);
    void pop_range(void *&start, void *&end, size_t n);

    size_t pop_batch(void **batch, size_t n);

    bool empty() const;
    size_t size() const;
    size_t &max_size();

  private:
    void *free_list_ = nullptr; // 链表头。
    size_t size_ = 0;           // 当前空闲对象数量。
    size_t max_size_ = 1;       // 批量回填/回收的动态阈值。
};

} // namespace zmalloc

#endif // ZMALLOC_INTERNAL_FREE_LIST_H_
