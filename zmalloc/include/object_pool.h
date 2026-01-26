#ifndef ZMALLOC_OBJECT_POOL_H_
#define ZMALLOC_OBJECT_POOL_H_

/**
 * @file object_pool.h
 * @brief 定长内存池，用于高效分配固定大小的对象（如 Span）
 */

#include "common.h"
#include "zmalloc_noncopyable.h"

#include <algorithm>

namespace zmalloc {

/**
 * @brief 定长内存池模板类
 * @tparam T 管理的对象类型
 */
template <typename T> class ObjectPool : public NonCopyable {
public:
  /**
   * @brief 分配一个对象
   * @return 对象指针
   */
  T *allocate() {
    T *obj = nullptr;

    // 优先复用释放回来的对象
    if (free_list_ != nullptr) {
      obj = static_cast<T *>(free_list_);
      free_list_ = next_obj(free_list_);
    } else {
      // 保证对象能存储一个指针，同时保证对齐满足 next_obj(void**) 的要求。
      const size_t obj_align = std::max(alignof(T), alignof(void *));
      size_t obj_size = sizeof(T) < sizeof(void *) ? sizeof(void *) : sizeof(T);
      obj_size = (obj_size + obj_align - 1) & ~(obj_align - 1);

      auto ensure_block = [&]() {
        if (memory_ == nullptr || remain_bytes_ < obj_size) {
          remain_bytes_ = 128 * 1024;
          memory_ =
              static_cast<char *>(system_alloc(remain_bytes_ >> PAGE_SHIFT));
        }
      };

      ensure_block();

      // 对齐当前切分指针
      uintptr_t cur = reinterpret_cast<uintptr_t>(memory_);
      uintptr_t aligned =
          (cur + obj_align - 1) & ~(static_cast<uintptr_t>(obj_align - 1));
      size_t padding = static_cast<size_t>(aligned - cur);

      if (remain_bytes_ < padding + obj_size) {
        // 当前 block 剩余不足以完成对齐 + 分配，申请新 block 再来一次。
        memory_ = nullptr;
        remain_bytes_ = 0;
        ensure_block();
        cur = reinterpret_cast<uintptr_t>(memory_);
        aligned =
            (cur + obj_align - 1) & ~(static_cast<uintptr_t>(obj_align - 1));
        padding = static_cast<size_t>(aligned - cur);
      }

      memory_ += padding;
      remain_bytes_ -= padding;

      obj = reinterpret_cast<T *>(memory_);
      memory_ += obj_size;
      remain_bytes_ -= obj_size;
    }

    // 定位 new，调用构造函数
    new (obj) T;
    return obj;
  }

  /**
   * @brief 释放一个对象
   * @param obj 对象指针
   */
  void deallocate(T *obj) {
    // 调用析构函数
    obj->~T();

    // 头插到自由链表
    next_obj(obj) = free_list_;
    free_list_ = obj;
  }

private:
  char *memory_ = nullptr;    // 大块内存指针
  size_t remain_bytes_ = 0;   // 剩余字节数
  void *free_list_ = nullptr; // 回收的对象自由链表
};

} // namespace zmalloc

#endif // ZMALLOC_OBJECT_POOL_H_
