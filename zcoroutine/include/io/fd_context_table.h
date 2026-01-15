#ifndef ZCOROUTINE_FD_CONTEXT_TABLE_H_
#define ZCOROUTINE_FD_CONTEXT_TABLE_H_

#include <vector>

#include "io/fd_context.h"
#include "sync/rw_mutex.h"
#include "util/noncopyable.h"

namespace zcoroutine {

/**
 * @brief FD到FdContext的线程安全映射表
 *
 * 管理 fd -> FdContext 的映射关系，提供线程安全的访问接口。
 * 使用读写锁优化读多写少的访问模式。
 */
class FdContextTable : public NonCopyable {
public:
  /**
   * @brief 构造函数
   * @param initial_capacity 初始容量，默认64
   */
  explicit FdContextTable(size_t initial_capacity = 64);

  ~FdContextTable() = default;

  /**
   * @brief 获取fd对应的FdContext（只读）
   * @param fd 文件描述符
   * @return FdContext指针，不存在返回nullptr
   */
  FdContext::ptr get(int fd);

  /**
   * @brief 获取或创建fd对应的FdContext
   * @param fd 文件描述符
   * @return FdContext指针，不存在则创建
   */
  FdContext::ptr get_or_create(int fd);

  /**
   * @brief 当前表大小（调试用）
   */
  size_t size() const;

private:
  /**
   * @brief 扩容并创建FdContext（需持有写锁）
   * @param fd 文件描述符
   * @return 新创建的FdContext
   */
  FdContext::ptr expand_and_create(int fd);

private:
  mutable RWMutex mutex_;                // 读写锁
  std::vector<FdContext::ptr> contexts_; // fd -> FdContext 映射
};

} // namespace zcoroutine

#endif // ZCOROUTINE_FD_CONTEXT_TABLE_H_
