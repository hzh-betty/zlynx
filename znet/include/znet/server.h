#ifndef ZNET_SERVER_H_
#define ZNET_SERVER_H_

#include "noncopyable.h"

#include <atomic>

namespace znet {

/**
 * @brief 服务器抽象基类，负责统一管理服务启动/停止状态机。
 *
 * 设计目的：
 * 1. 将“只能启动一次、只能停止一次”的并发控制逻辑集中到基类。
 * 2. 派生类只关心真正的资源初始化与回收（do_start/do_stop）。
 *
 * 线程安全说明：
 * - start()/stop() 内部使用原子状态位，支持并发调用。
 * - do_start()/do_stop() 由派生类实现，需自行保证内部线程安全。
 */
class Server : public NonCopyable {
 public:
  virtual ~Server() = default;

  /**
  * @brief 启动服务。
  * @return true 表示当前实例处于运行态；false 表示启动流程失败。
  *
  * 语义细节：
  * - 如果服务已在运行，直接返回 true（幂等）。
  * - 如果 do_start() 失败，基类会自动回滚 running_ 状态。
  */
  bool start();

  /**
  * @brief 停止服务。
  *
  * 语义细节：
  * - 如果服务已经停止，直接返回（幂等）。
  * - 仅当状态从 running 变为 stopped 时，才会调用 do_stop()。
  */
  void stop();

  /**
  * @brief 查询服务是否运行中。
  */
  bool is_running() const { return running_.load(); }

 protected:
  /**
  * @brief 由派生类实现的“真正启动逻辑”。
  * @return true 表示资源初始化成功。
  */
  virtual bool do_start() = 0;

  /**
  * @brief 由派生类实现的“真正停止逻辑”。
  */
  virtual void do_stop() = 0;

 private:
  std::atomic<bool> running_{false};
};

}  // namespace znet

#endif  // ZNET_SERVER_H_
