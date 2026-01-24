#include "scheduler.h"

#include <memory>
#include <utility>

#include "fiber_pool.h"
#include "hook.h"
#include "thread_context.h"
#include "work_stealing_queue.h"
#include "zcoroutine_logger.h"

namespace zcoroutine {

Scheduler::Scheduler(int thread_count, std::string name, bool use_shared_stack)
    : name_(std::move(name)), pool_(thread_count, name_), stopping_(true),
      use_shared_stack_(use_shared_stack) {
  // 共享栈将在每个worker线程的run()中独立创建
  ZCOROUTINE_LOG_INFO(
      "Scheduler[{}] created with thread_count={}, shared_stack={}", name_,
      pool_.thread_count(), use_shared_stack_);
}

Scheduler::~Scheduler() {
  ZCOROUTINE_LOG_DEBUG("Scheduler[{}] destroying", name_);
  stop();
  ZCOROUTINE_LOG_INFO("Scheduler[{}] destroyed", name_);
}

void Scheduler::enqueue(Task &&task) {
  if (!task.is_valid()) {
    ZCOROUTINE_LOG_WARN("Scheduler::enqueue received invalid task");
    return;
  }

  // 若当前线程正在运行本 Scheduler（即 worker
  // 线程），优先投递到本线程本地队列。
  WorkStealingQueue *q = nullptr;
  if (Scheduler::get_this() == this) {
    q = ThreadContext::get_work_queue();
  }

  // 外部线程/IO 线程投递：优先选择位图中为 0 的
  // worker（通常表示未达到“可窃取”阈值）。
  if (!q) {
    const size_t start = static_cast<size_t>(
        pool_.next_rr());
    const int preferred = pool_.bitmap().find_non_stealable(start);

    // 1) 优先：位图为 0 的 worker
    if (preferred >= 0) {
      q = pool_.get_next_queue(preferred);
    }

    // 2) fallback：扫描找到任意已注册队列（可能处于启动/退出边界）
    if (!q) {
      const int n = pool_.thread_count();
      for (int k = 0; k < n; ++k) {
        const int idx = (static_cast<int>(start) + k) % n;
        if (auto *cand = pool_.get_next_queue(idx)) {
          q = cand;
          break;
        }
      }
    }
  }

  if (!q) {
    ZCOROUTINE_LOG_ERROR(
        "Scheduler[{}] enqueue failed: no available worker queue", name_);
    return;
  }

  // 先增加待处理计数，再入队：避免 stop() 期间 schedule_loop 因
  // pending_tasks_=0 提前退出。
  pending_tasks_.fetch_add(1, std::memory_order_relaxed);
  q->push(std::move(task));
}

void Scheduler::start() {
  stopping_ = false;

  // pool_ 内部会忽略重复 start
  if (pool_.thread_count() <= 0) {
    ZCOROUTINE_LOG_WARN("Scheduler[{}] start called with non-positive threads",
                       name_);
  }

  ZCOROUTINE_LOG_INFO("Scheduler[{}] starting with {} threads...", name_,
                      pool_.thread_count());

  pool_.start([this](int worker_id) {
    // 设置线程的调度器
    set_this(this);
    ThreadContext::set_worker_id(worker_id);
    ThreadContext::set_work_queue(pool_.local_queue(worker_id));

    ZCOROUTINE_LOG_DEBUG("Scheduler[{}] worker thread {} started", name_,
                         worker_id);
    this->run();
    ZCOROUTINE_LOG_DEBUG("Scheduler[{}] worker thread {} exited", name_,
                         worker_id);
  });

  ZCOROUTINE_LOG_INFO("Scheduler[{}] started successfully with {} threads",
                      name_, pool_.thread_count());
}

void Scheduler::stop() {
  if (stopping_) {
    ZCOROUTINE_LOG_DEBUG("Scheduler[{}] already stopping, skip", name_);
    return; // 已经在停止中
  }

  ZCOROUTINE_LOG_INFO("Scheduler[{}] stopping with {} pending tasks...", name_,
                      pending_tasks_.load(std::memory_order_relaxed));

  // 先设置停止标志，避免 stop 期间被持续调度新任务导致无法退出
  stopping_ = true;

  // 停止并唤醒所有队列等待者
  pool_.stop();

  ZCOROUTINE_LOG_INFO("Scheduler[{}] stopped successfully", name_);
}

void Scheduler::schedule(const Fiber::ptr &fiber) {
  if (!fiber) {
    ZCOROUTINE_LOG_WARN("Scheduler[{}]::schedule received null fiber", name_);
    return;
  }

  ZCOROUTINE_LOG_DEBUG("Scheduler[{}] scheduled fiber name={}, id={}", name_,
                       fiber->name(), fiber->id());

  enqueue(Task(fiber));
}

void Scheduler::schedule(Fiber::ptr &&fiber) {
  if (!fiber) {
    ZCOROUTINE_LOG_WARN("Scheduler[{}]::schedule received null fiber", name_);
    return;
  }

  ZCOROUTINE_LOG_DEBUG("Scheduler[{}] scheduled fiber name={}, id={}", name_,
                       fiber->name(), fiber->id());

  enqueue(Task(std::move(fiber)));
}

Scheduler *Scheduler::get_this() { return ThreadContext::get_scheduler(); }

void Scheduler::set_this(Scheduler *scheduler) {
  ThreadContext::set_scheduler(scheduler);
}

void Scheduler::run() {
  ZCOROUTINE_LOG_DEBUG("Scheduler[{}] worker thread entering run loop", name_);

  // 为当前 worker 线程创建并设置 main_fiber（保存线程原始上下文）。
  // 注意：主线程不参与调度，不应在 Scheduler 构造函数中污染创建线程的 TLS。
  Fiber::ptr main_fiber(new Fiber());
  ThreadContext::set_main_fiber(main_fiber);
  ThreadContext::set_current_fiber(main_fiber);

  // 开启hook，让worker线程可以使用协程版的系统调用
  set_hook_enable(true);

  // 创建并发布本线程的 work-stealing 队列
  const int id = ThreadContext::get_worker_id();
  if (id >= 0 && id < pool_.thread_count()) {
    WorkStealingQueue *q_ptr = ThreadContext::get_work_queue();
    pool_.register_work_queue(id, q_ptr);
    ZCOROUTINE_LOG_DEBUG("Scheduler[{}] registered work queue for worker_id={}",
                         name_, id);

    // 绑定全局位图（H/L 水位，避免每次 push/pop 触发位图写入）。
    // H 要明显大于批处理大小，L 要明显小于 H。
    static constexpr size_t kHighWatermark = 256;
    static constexpr size_t kLowWatermark = 64;
    q_ptr->bind_bitmap(&pool_.bitmap(), id, kHighWatermark, kLowWatermark);
  }

  // 创建调度器协程，它将运行调度循环
  // 注意：scheduler_fiber 必须使用独立栈，因为它负责协程切换
  // 如果使用共享栈，切换时栈内容会被覆盖导致段错误
  auto scheduler_fiber = std::make_shared<Fiber>(
      [this]() { this->schedule_loop(); }, StackAllocator::kDefaultStackSize,
      "scheduler", false);

  // 如果使用共享栈模式，为当前线程创建独立的共享栈
  // 每个线程都有自己独立的SharedStack，避免多线程竞争
  if (use_shared_stack_) {
    ThreadContext::set_stack_mode(StackMode::kShared);
    ThreadContext::get_shared_stack();
  }
  ThreadContext::set_scheduler_fiber(scheduler_fiber);

  ZCOROUTINE_LOG_DEBUG("Scheduler[{}] main_fiber and scheduler_fiber created",
                       name_);

  // 启动调度器协程
  try {
    scheduler_fiber->resume();
  } catch (const std::exception &e) {
    ZCOROUTINE_LOG_ERROR(
        "Scheduler[{}] fiber execution exception: name={}, id={}, error={}",
        name_, scheduler_fiber->name(), scheduler_fiber->id(), e.what());
  } catch (...) {
    ZCOROUTINE_LOG_ERROR(
        "Scheduler[{}] fiber execution unknown exception: name={}, id={}",
        name_, scheduler_fiber->name(), scheduler_fiber->id());
  }

  // 调度器协程结束后，清理
  ThreadContext::set_scheduler_fiber(nullptr);
  ThreadContext::set_main_fiber(nullptr);
  ThreadContext::set_current_fiber(nullptr);

  // 如果使用了共享栈模式，重置线程本地配置
  if (use_shared_stack_) {
    ThreadContext::reset_shared_stack_config();
  }

  ZCOROUTINE_LOG_DEBUG("Scheduler[{}] worker thread exiting run loop", name_);
}

void Scheduler::schedule_loop() {
  ZCOROUTINE_LOG_DEBUG("Scheduler[{}] schedule_loop starting", name_);

  // 批量处理优化：减少锁竞争
  static constexpr size_t kBatchSize = 8;
  Task tasks[kBatchSize];
  std::vector<Task> stolen_buf;
  const int self_id = ThreadContext::get_worker_id();
  WorkStealingQueue *local_queue = ThreadContext::get_work_queue();
  const int worker_count = pool_.thread_count();

  while (true) {
    // 如果正在停止且无待处理任务，则退出循环
    if (stopping_ && pending_tasks_.load(std::memory_order_relaxed) == 0) {
      break;
    }

    size_t batch_count = 0;

    // 1) 先从本地队列批量取任务（LIFO）
    if (local_queue) {
      batch_count = local_queue->pop_batch(tasks, kBatchSize);
    }
    if (batch_count > 0) {
      ZCOROUTINE_LOG_DEBUG(
          "Scheduler[{}] worker_id={} fetched {} tasks from local queue", name_,
          self_id, batch_count);
    }

    // 2) 本地为空则尝试批量窃取：使用全局位图引导 victim 选择。
    if (batch_count == 0 && worker_count > 1 && self_id >= 0) {
      const int victim = pool_.bitmap().find_victim(self_id);
      if (victim >= 0) {
        WorkStealingQueue *victim_q = pool_.get_next_queue(victim);
        if (victim_q) {
          const size_t victim_size = victim_q->approx_size();
          if (batch_count == 0 && victim_size > 0) {
            // 一次性窃取受害者队列的一半任务（向上取整）。
            const size_t target = (victim_size + 1) / 2;
            if (stolen_buf.size() < target) {
              stolen_buf.resize(target);
            }

            const size_t n = victim_q->steal_batch(stolen_buf.data(), target);
            if (n > 0) {
              ZCOROUTINE_LOG_DEBUG(
                  "Scheduler[{}] worker_id={} stole {} tasks from victim {} (target={})",
                  name_, self_id, n, victim, target);
              for (size_t i = 0; i < n; ++i) {
                if (batch_count < kBatchSize) {
                  tasks[batch_count++] = std::move(stolen_buf[i]);
                } else if (local_queue) {
                  local_queue->push(std::move(stolen_buf[i]));
                }
                stolen_buf[i].reset();
              }
            }
          }
        }
      }
    }

    // 3) 没有任务则在本地队列上等待
    if (batch_count == 0) {
      // 这里用短超时轮询来兼顾“被动等待本地投递”和“周期性尝试 steal”。
      const int timeout_ms = pool_.bitmap().any() ? 1 : 100;
      batch_count = local_queue->wait_pop_batch(tasks, kBatchSize, timeout_ms);
      if (batch_count == 0) {
        continue;
      }
    }

    pending_tasks_.fetch_sub(batch_count, std::memory_order_relaxed);

    // 批量执行任务
    for (size_t i = 0; i < batch_count; ++i) {
      Task &task = tasks[i];

      if (!task.is_valid()) {
        continue;
      }

      // 执行任务
      if (task.fiber) {
        // 执行协程
        const Fiber::ptr fiber = task.fiber;

        ZCOROUTINE_LOG_DEBUG(
          "Scheduler[{}] executing fiber name={}, id={}", name_,
          fiber->name(), fiber->id());

        try {
          fiber->resume();
        } catch (const std::exception &e) {
          ZCOROUTINE_LOG_ERROR("Scheduler[{}] fiber execution exception: "
                               "name={}, id={}, error={}",
                               name_, fiber->name(), fiber->id(), e.what());
        } catch (...) {
          ZCOROUTINE_LOG_ERROR(
              "Scheduler[{}] fiber execution unknown exception: name={}, id={}",
              name_, fiber->name(), fiber->id());
        }

        // 如果协程终止，归还到池中
        if (fiber->state() == Fiber::State::kTerminated) {
          ZCOROUTINE_LOG_DEBUG("Scheduler[{}] fiber terminated: name={}, id={}",
                               name_, fiber->name(), fiber->id());

          // 尝试归还协程到池中
          bool returned = FiberPool::get_instance().return_fiber(fiber);
          if (returned) {
            ZCOROUTINE_LOG_DEBUG("Scheduler[{}] fiber returned to pool: "
                                 "name={}, id={}",
                                 name_, fiber->name(), fiber->id());
          } else {
            ZCOROUTINE_LOG_DEBUG("Scheduler[{}] fiber not returned to pool "
                                 "(pool full or invalid): name={}, id={}",
                                 name_, fiber->name(), fiber->id());
          }
        }
        // 如果协程挂起，说明在等待外部事件（IO、定时器等）
        else if (fiber->state() == Fiber::State::kSuspended) {
          ZCOROUTINE_LOG_DEBUG("Scheduler[{}] fiber suspended, waiting for "
                               "external event: name={}, id={}",
                               name_, fiber->name(), fiber->id());
        }
      } else if (task.callback) {
        auto cb = std::move(task.callback);
        task.callback = nullptr;
        try {
          cb();
        } catch (const std::exception &e) {
          ZCOROUTINE_LOG_ERROR("Scheduler[{}] callback exception: error={}",
                               name_, e.what());
        } catch (...) {
          ZCOROUTINE_LOG_ERROR("Scheduler[{}] callback unknown exception",
                               name_);
        }
      }

      // 清理任务
      task.reset();
    }
  }

  ZCOROUTINE_LOG_DEBUG("Scheduler[{}] schedule_loop ended", name_);
}

} // namespace zcoroutine
