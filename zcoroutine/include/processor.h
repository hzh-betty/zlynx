#ifndef ZCOROUTINE_PROCESSOR_H_
#define ZCOROUTINE_PROCESSOR_H_

#include "work_stealing_queue.h"

namespace zcoroutine {

/**
 * @brief Processor（P）抽象：每个 P 拥有一个本地运行队列（runq）。
 *
 * 当前阶段保持 1:1 绑定：每个 worker 线程（M）固定绑定一个 Processor（P）。
 */
struct Processor {
  explicit Processor(int id) : id(id) {}

  int id{-1};
  WorkStealingQueue run_queue;
};

} // namespace zcoroutine

#endif // ZCOROUTINE_PROCESSOR_H_
