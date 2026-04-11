#ifndef ZNET_ACCEPTOR_H_
#define ZNET_ACCEPTOR_H_

#include <atomic>
#include <functional>
#include <memory>

#include "znet/address.h"
#include "znet/internal/noncopyable.h"
#include "znet/socket.h"

namespace znet {

/**
 * @brief TCP 连接接入器：负责监听端口并接受新连接。
 *
 * 工作流程：
 * 1. start() 创建监听 socket，并完成 bind/listen。
 * 2. 在协程中执行 accept_loop()，持续接收客户端连接。
 * 3. 每当接入成功，触发 AcceptCallback 将 Socket 交给上层。
 *
 * 使用约束：
 * - 必须以 shared_ptr 方式持有对象后再调用 start()，
 *   因为内部会在协程中使用 shared_from_this()。
 */
class Acceptor : public std::enable_shared_from_this<Acceptor>,
                 public NonCopyable {
  public:
    using ptr = std::shared_ptr<Acceptor>;
    using AcceptCallback = std::function<void(Socket::ptr)>;

    /**
     * @param listen_address 监听地址（IP + port）。
     * @param backlog 内核监听队列长度。
     */
    explicit Acceptor(Address::ptr listen_address, int backlog = SOMAXCONN);
    ~Acceptor();

    /**
     * @brief 启动接入器。
     * @return true 表示启动成功或已在运行；false 表示启动失败。
     */
    bool start();

    /**
     * @brief 停止接入器，关闭监听 socket。
     */
    void stop();

    /**
     * @brief 接入器是否处于运行状态。
     */
    bool is_running() const { return running_.load(); }

    /**
     * @brief 设置“新连接到达”回调。
     * @param callback 接入成功后被调用，参数为客户端 socket。
     */
    void set_accept_callback(AcceptCallback callback) {
        accept_callback_ = std::move(callback);
    }

    /**
     * @brief 获取监听地址。
     */
    Address::ptr listen_address() const { return listen_address_; }

    /**
     * @brief 获取监听 socket。
     */
    Socket::ptr listen_socket() const { return listen_socket_; }

  private:
    /**
     * @brief 接收循环主体。
     *
     * 循环直到以下任一条件满足：
     * - running_ 被置为 false；
     * - 监听 socket 被关闭或失效；
     * - 出现不可恢复错误。
     */
    void accept_loop();

  private:
    Address::ptr listen_address_;
    int backlog_; // 监听队列长度
    Socket::ptr listen_socket_;
    AcceptCallback accept_callback_;
    std::atomic<bool> running_{false};
};

} // namespace znet

#endif // ZNET_ACCEPTOR_H_
