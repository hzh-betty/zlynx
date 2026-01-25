#ifndef ZCOROUTINE_FD_CONTEXT_H_
#define ZCOROUTINE_FD_CONTEXT_H_

#include <sys/epoll.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

#include "zcoroutine_noncopyable.h"

namespace zcoroutine {

class Fiber;
/**
 * @brief 文件描述符上下文类
 *
 * 管理单个文件描述符的IO事件和对应的协程回调。
 * 支持读、写两种事件类型，每种事件可以关联一个协程。
 */
class Channel : public NonCopyable {
public:
  using ptr = std::shared_ptr<Channel>;

  /**
   * @brief 事件类型定义
   */
  enum Event {
    kNone = 0x0,      // 无事件
    kRead = EPOLLIN,  // 读事件
    kWrite = EPOLLOUT // 写事件
  };

  /**
   * @brief 将Event枚举转换为字符串
   * @param event 事件类型
   * @return 事件类型的字符串表示
   */
  static const char *event_to_string(Event event) {
    switch (event) {
    case kNone:
      return "NONE";
    case kRead:
      return "READ";
    case kWrite:
      return "WRITE";
    default:
      return "UNKNOWN";
    }
  }

  /**
   * @brief 事件上下文结构
   * 优化布局：把指针放在一起，利用空间局部性
   */
  struct EventContext {
    std::function<void()> callback; // 事件回调函数 - 16字节
    std::shared_ptr<Fiber> fiber;   // 等待该事件的协程 - 16字节
  };

  explicit Channel(int fd);
  ~Channel() = default;

  /**
   * @brief 获取文件描述符
   */
  int fd() const { return fd_; }

  /**
   * @brief 获取当前事件
   */
  int events() const { return events_.load(std::memory_order_relaxed); }

  struct PopResult {
    std::function<void()> callback;
    std::shared_ptr<Fiber> fiber;
    int remaining_events = kNone;
    bool had_event = false;
  };

  // 线程安全：取出并清空指定事件的 callback/fiber，同时清除事件位。
  PopResult pop_event(Event event);

  /**
   * @brief 添加事件
   * @param event 事件类型（kRead或kWrite）
   * @return 添加后的事件
   */
  int add_event(Event event);

  /**
   * @brief 删除事件
   * @param event 事件类型
   * @return 删除后的事件
   */
  int del_event(Event event);

  /**
   * @brief 取消事件
   * @param event 事件类型
   * @return 取消后的事件
   */
  int cancel_event(Event event);

  /**
   * @brief 取消所有事件
   */
  void cancel_all();

  /**
   * @brief 触发事件
   * @param event 事件类型
   */
  void trigger_event(Event event);

  /**
   * @brief 获取事件上下文
   * @param event 事件类型
   * @return 事件上下文引用
   */
  EventContext &get_event_context(Event event);

  /**
   * @brief 重置事件上下文
   * @param ctx 事件上下文
   */
  void reset_event_context(EventContext &ctx);

private:
  int fd_;                         // 文件描述符 - 4字节
  std::atomic<int> events_{kNone}; // 当前注册的事件

  // 事件上下文：紧接着热数据
  EventContext read_ctx_;  // 读事件上下文 - 32字节
  EventContext write_ctx_; // 写事件上下文 - 32字节

  // 冷数据：竞争保护，放在最后降低false sharing
  alignas(64) std::mutex mutex_; // 保护上下文的互斥锁 - 独占缓存行
};

} // namespace zcoroutine

#endif // ZCOROUTINE_FD_CONTEXT_H_
