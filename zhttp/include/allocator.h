#ifndef ZHTTP_ALLOCATOR_H_
#define ZHTTP_ALLOCATOR_H_

#include <cstddef>

namespace zhttp {

/**
 * @brief 内存分配器
 * 提供统一的内存分配接口，暂时使用 malloc/free 实现
 * 为未来的内存池预留接口
 */
class Allocator {
public:
  /**
   * @brief 分配内存
   * @param size 需要分配的字节数
   * @return 分配的内存指针，失败返回 nullptr
   */
  static void *allocate(size_t size);

  /**
   * @brief 重新分配内存
   * @param ptr 原内存指针
   * @param old_size 原内存大小（字节）
   * @param new_size 新内存大小（字节）
   * @return 新的内存指针，失败返回 nullptr
   */
  static void *reallocate(void *ptr, size_t old_size, size_t new_size);

  /**
   * @brief 释放内存
   * @param ptr 内存指针
   * @param size 内存大小（字节）
   * @note ptr 为 nullptr 时忽略
   */
  static void deallocate(void *ptr, size_t size);

  // 禁止实例化
  Allocator() = delete;
  ~Allocator() = delete;
};

} // namespace zhttp

#endif // ZHTTP_ALLOCATOR_H_
