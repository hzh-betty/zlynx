#ifndef ZCOROUTINE_STEALABLE_QUEUE_BITMAP_H_
#define ZCOROUTINE_STEALABLE_QUEUE_BITMAP_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "zcoroutine_noncopyable.h"

namespace zcoroutine {

/**
 * @brief 可窃取队列提示位图
 *
 * 用途：记录“哪些 worker 的本地 WorkStealingQueue 任务足够多，值得被窃取”。
 * 第 i 位为 1 表示 worker i 的队列处于可窃取状态（通常意味着队列长度超过高水位阈值）。
 *
 * @note
 * - 位图为启发式信息：允许与真实队列长度存在短暂偏差。
 * - 位图按 64bit 分段，每段使用 cacheline 独占对齐的 atomic<uint64_t>，降低 false sharing。
 */
class StealableQueueBitmap : public NonCopyable {
public:
  /**
   * @brief 构造函数
   * @param worker_count worker 数量
   */
  explicit StealableQueueBitmap(size_t worker_count);

  /**
   * @brief 获取 worker 数量
   * @return worker 数量
   */
  size_t worker_count() const { return worker_count_; }

  /**
   * @brief 将 worker_id 对应位设置为 1
   * @param worker_id worker id
   */
  void set(size_t worker_id);

  /**
   * @brief 将 worker_id 对应位清零
   * @param worker_id worker id
   */
  void clear(size_t worker_id);

  /**
   * @brief 测试某个 worker 位是否为 1（可窃取）
   */
  bool test(size_t worker_id) const;

  /**
   * @brief 判断位图是否存在任意可窃取 worker
   */
  bool any() const;

  /**
   * @brief 从 start 开始扫描，返回一个位为 0 的 worker（倾向于“更空闲/未达到可窃取阈值”）。
   * @return worker id；若未找到返回 -1
   */
  int find_non_stealable(size_t start) const;

  /**
   * @brief 从 (self_id + 1) % N 开始循环扫描，返回一个可窃取 victim
   * @param self_id 当前 worker id
   * @return victim id；若未找到返回 -1
   */
  int find_victim(int self_id) const;

private:
  struct alignas(64) CacheLineU64 {
    std::atomic<uint64_t> v{0};
  };

  /**
   * @brief 在 [from, to) 范围内查找可窃取 victim
   * @param from 起始 worker id（包含）
   * @param to 结束 worker id（不包含）
   * @param self_id 当前 worker id（用于排除自己）
   * @return victim id；若未找到返回 -1
   */
  int find_in_range(size_t from, size_t to, size_t self_id) const;

  int find_zero_in_range(size_t from, size_t to) const;

  size_t worker_count_{0};
  std::vector<CacheLineU64> words_;
};

} // namespace zcoroutine

#endif // ZCOROUTINE_STEALABLE_QUEUE_BITMAP_H_
