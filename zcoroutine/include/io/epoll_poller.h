#ifndef ZCOROUTINE_EPOLL_POLLER_H_
#define ZCOROUTINE_EPOLL_POLLER_H_

#include <sys/epoll.h>

#include <memory>
#include <vector>

#include "util/noncopyable.h"

namespace zcoroutine {

/**
 * @brief Epoll封装类
 *
 * 封装Linux epoll系统调用，提供IO事件的添加、删除、等待功能。
 * 使用边缘触发模式。
 */
class EpollPoller : public NonCopyable {
public:
  using ptr = std::shared_ptr<EpollPoller>;

  /**
   * @brief 构造函数
   * @param max_events 最大事件数量
   */
  explicit EpollPoller(size_t max_events = 256);

  /**
   * @brief 析构函数
   */
  ~EpollPoller();

  /**
   * @brief 添加事件
   * @param fd 文件描述符
   * @param events 事件类型（EPOLLIN/EPOLLOUT等）
   * @param data 用户数据（通常是FdContext指针）
   * @return 成功返回0，失败返回-1
   */
  int add_event(int fd, int events, void *data);

  /**
   * @brief 修改事件
   * @param fd 文件描述符
   * @param events 新的事件类型
   * @param data 用户数据
   * @return 成功返回0，失败返回-1
   */
  int mod_event(int fd, int events, void *data);

  /**
   * @brief 删除事件
   * @param fd 文件描述符
   * @return 成功返回0，失败返回-1
   */
  int del_event(int fd);

  /**
   * @brief 等待IO事件
   * @param timeout_ms 超时时间（毫秒），-1表示永久等待
   * @param events 输出参数，存储就绪的事件
   * @return 就绪事件数量，失败返回-1
   */
  int wait(int timeout_ms, std::vector<epoll_event> &events);

  /**
   * @brief 获取epoll文件描述符
   */
  int epoll_fd() const { return epoll_fd_; }

private:
  int epoll_fd_;                    // epoll文件描述符
  std::vector<epoll_event> events_; // 事件数组
  size_t max_events_;               // 最大事件数量
};

} // namespace zcoroutine

#endif // ZCOROUTINE_EPOLL_POLLER_H_
