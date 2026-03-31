#include "znet/server.h"

#include "znet/znet_logger.h"

namespace znet {

// 统一启动入口：基类负责并发状态切换，派生类专注资源初始化。
bool Server::start() {
  bool expected = false;
  // compare_exchange 保证只有第一个调用者进入 do_start()。
  if (!running_.compare_exchange_strong(expected, true)) {
    ZNET_LOG_DEBUG("Server::start skipped because server is already running");
    return true;
  }

  ZNET_LOG_INFO("Server::start begin");

  if (!do_start()) {
    // 派生类启动失败时回滚运行态，保持对象可重试启动。
    running_.store(false);
    ZNET_LOG_ERROR("Server::start failed and rolled back running state");
    return false;
  }

  ZNET_LOG_INFO("Server::start success");
  return true;
}

// 统一停止入口：只允许从 running->stopped 的状态跃迁触发真正停机。
void Server::stop() {
  bool expected = true;
  if (!running_.compare_exchange_strong(expected, false)) {
    ZNET_LOG_DEBUG("Server::stop skipped because server is not running");
    return;
  }

  ZNET_LOG_INFO("Server::stop begin");
  // do_stop() 由派生类负责资源回收顺序与异常兜底。
  do_stop();
  ZNET_LOG_INFO("Server::stop completed");
}

}  // namespace znet
