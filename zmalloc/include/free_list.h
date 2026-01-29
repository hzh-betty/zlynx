#ifndef ZMALLOC_FREE_LIST_H_
#define ZMALLOC_FREE_LIST_H_

#include <cstddef>

namespace zmalloc {

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

  size_t pop_batch(void **batch, size_t n);

  bool empty() const;
  size_t size() const;
  size_t &max_size();

private:
  void *free_list_ = nullptr;
  size_t size_ = 0;
  size_t max_size_ = 1;
};

} // namespace zmalloc

#endif // ZMALLOC_FREE_LIST_H_
