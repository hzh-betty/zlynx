#ifndef ZCOROUTINE_IO_SCHEDULER_H_
#define ZCOROUTINE_IO_SCHEDULER_H_

#include <memory>

#include "io/epoll_poller.h"
#include "io/fd_context.h"
#include "io/fd_context_table.h"
#include "scheduling/scheduler.h"
#include "timer/timer_manager.h"

namespace zcoroutine {
/**
 * @brief IO调度器
 *
 * 继承Scheduler，扩展IO事件和定时器功能。
 */
class IoScheduler : public Scheduler {
public:
  using ptr = std::shared_ptr<IoScheduler>;

  /**
   * @brief 构造函数
   * @param thread_count 线程数量
   * @param name 调度器名称
   * @param use_shared_stack 是否使用共享栈
   */
  IoScheduler(int thread_count, const std::string &name,
              bool use_shared_stack = false);

  /**
   * @brief 析构函数
   */
  ~IoScheduler() override;

  /**
   * @brief 启动IO调度器
   */
  void start();

  /**
   * @brief 停止IO调度器
   */
  void stop();

  /**
   * @brief 添加IO事件
   * @param fd 文件描述符
   * @param event 事件类型（FdContext::kRead或kWrite）
   * @param callback 事件回调函数
   * @return 成功返回0，失败返回-1
   */
  int add_event(int fd, FdContext::Event event,
                std::function<void()> callback = nullptr);

  /**
   * @brief 删除IO事件
   * @param fd 文件描述符
   * @param event 事件类型
   * @return 成功返回0，失败返回-1
   */
  int del_event(int fd, FdContext::Event event);

  /**
   * @brief 取消IO事件
   * @param fd 文件描述符
   * @param event 事件类型
   * @return 成功返回0，失败返回-1
   */
  int cancel_event(int fd, FdContext::Event event);

  /**
   * @brief 取消文件描述符上的所有事件
   * @param fd 文件描述符
   * @return 成功返回0，失败返回-1
   */
  int cancel_all(int fd);

  /**
   * @brief 触发IO事件（先触发回调再删除事件）
   * @param fd 文件描述符
   * @param event 事件类型
   *
   */
  void trigger_event(int fd, FdContext::Event event);

  /**
   * @brief 添加定时器
   * @param timeout 超时时间（毫秒）
   * @param callback 定时器回调
   * @param recurring 是否循环
   * @return 定时器指针
   */
  Timer::ptr add_timer(uint64_t timeout, std::function<void()> callback,
                       bool recurring = false);

  /**
   * @brief 添加条件定时器
   * @param timeout 超时时间（毫秒）
   * @param callback 定时器回调
   * @param weak_cond 弱引用条件
   * @param recurring 是否循环
   * @return 定时器指针
   */
  Timer::ptr add_condition_timer(uint64_t timeout,
                                 std::function<void()> callback,
                                 std::weak_ptr<void> weak_cond,
                                 bool recurring = false);

  /**
   * @brief 获取TimerManager
   */
  TimerManager::ptr timer_manager() const { return timer_manager_; }

  /**
   * @brief 获取当前线程的IoScheduler
   * @return 当前线程的IoScheduler指针，如果不是IoScheduler则返回nullptr
   */
  static IoScheduler *get_this();

private:
  /**
   * @brief IO线程运行函数
   */
  void io_thread_func();

  /**
   * @brief 唤醒IO线程
   */
  void wake_up() const;

  // 获取/创建 fd 对应的 FdContext（epoll 事件上下文）
  FdContext::ptr get_fd_context(int fd, bool auto_create);

private:
  EpollPoller::ptr epoll_poller_;                    // Epoll封装
  TimerManager::ptr timer_manager_;                  // 定时器管理器
  std::unique_ptr<FdContextTable> fd_context_table_; // FdContext表
  std::unique_ptr<std::thread> io_thread_;           // IO线程
  int wake_fd_[2]{}; // 用于唤醒epoll的管道
};
} // namespace zcoroutine

#endif // ZCOROUTINE_IO_SCHEDULER_H_
