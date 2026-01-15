#ifndef ZNET_ALLOCATOR_H_
#define ZNET_ALLOCATOR_H_

#include <cstddef>

namespace znet {

/**
 * @brief 内存分配器
 * 参考 zcoroutine::StackAllocator 设计，提供统一的内存分配接口
 * 暂时使用 malloc/free 实现，为未来的内存池预留接口
 */
class Allocator {
public:
  /**
   * @brief 分配内存
   * @param size 需要分配的字节数
   * @return 分配的内存指针，失败返回 nullptr
   * @note 不进行内存清零，依赖内核 COW 机制
   */
  static void *allocate(size_t size);

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

} // namespace znet

#endif // ZNET_ALLOCATOR_H_
