#ifndef ZLYNX_ZNET_SRC_SERVER_ACCEPTOR_H_
#define ZLYNX_ZNET_SRC_SERVER_ACCEPTOR_H_

#include "zco/runtime.h"
#include "znet/transport/socket.h"
#include <atomic>
#include <functional>

namespace znet::detail {
/**
 * @brief 独占监听套接字，并跟踪执行接收循环的任务。
 *
 * request_stop 关闭监听资源以中断等待，join 只等待任务结束；
 * 所属 ServerRun 须存活至接收任务结束。
 */
class Acceptor {
  public:
    explicit Acceptor(Socket listener);
    static Result<Socket> listen(const Endpoint &endpoint, int backlog,
                                 bool reuse_port);

    // 提交的任务持有所属 ServerRun，确保 accept() 期间所有者仍存活。
    Result<void> start(const zco::Executor &executor, zco::Task task);

    /**
     * @brief 在所属执行器上接收连接，停止、接收失败或准入拒绝时退出。
     *
     * @param running 共享运行标志，须覆盖接收循环寿命。
     * @param on_accept 接管已接收套接字；返回 false 终止整个接收循环。
     * @param on_error 接收或回调异常的报告入口；停止引起的取消不报告。
     */
    void accept(const std::atomic<bool> &running,
                const std::function<bool(Socket)> &on_accept,
                const std::function<void(const Error &)> &on_error);
    void request_stop();
    void join();

  private:
    Socket listener_;
    zco::TaskHandle task_;
};
} // namespace znet::detail

#endif
