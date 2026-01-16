#pragma once

/**
 * @file object_pool.h
 * @brief 定长内存池，用于高效分配固定大小的对象（如 Span）
 */

#include "common.h"

namespace zmalloc {

/**
 * @brief 定长内存池模板类
 * @tparam T 管理的对象类型
 */
template <typename T> class ObjectPool {
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
      // 保证对象能存储一个指针
      size_t obj_size = sizeof(T) < sizeof(void *) ? sizeof(void *) : sizeof(T);

      // 剩余空间不足，向系统申请新的大块内存
      if (remain_bytes_ < obj_size) {
        remain_bytes_ = 128 * 1024;
        memory_ =
            static_cast<char *>(system_alloc(remain_bytes_ >> PAGE_SHIFT));
      }

      // 从大块内存切分
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
