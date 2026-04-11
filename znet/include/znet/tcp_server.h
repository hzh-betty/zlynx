#ifndef ZNET_TCP_SERVER_H_
#define ZNET_TCP_SERVER_H_

#include "acceptor.h"
#include "callbacks.h"
#include "noncopyable.h"
#include "tcp_connection.h"
#include "tls_context.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace zcoroutine {
class Scheduler;
}

namespace znet {

/**
 * @brief 基于协程调度器的 TCP 服务器实现。
 *
 * 核心职责：
 * 1. 管理 Acceptor 监听与新连接分发。
 * 2. 维护连接表，并在连接生命周期中触发业务回调。
 * 3. 将消息回调统一暴露为 Buffer 语义。
 */
class TcpServer : public std::enable_shared_from_this<TcpServer>,
                  public NonCopyable {
  public:
    using ptr = std::shared_ptr<TcpServer>;
    using ConnectionMap = std::unordered_map<int, TcpConnection::ptr>;

    explicit TcpServer(Address::ptr listen_address, int backlog = SOMAXCONN);
    ~TcpServer() = default;

    bool start();
    void stop();

    bool is_running() const { return running_.load(std::memory_order_acquire); }

    void set_thread_count(int thread_count) { thread_count_ = thread_count; }

    void set_on_message(MessageCallback callback) {
        on_message_callback_ = std::move(callback);
    }

    void set_on_close(CloseCallback callback) {
        on_close_callback_ = std::move(callback);
    }

    void set_on_connection(ConnectionCallback callback) {
        on_connection_callback_ = std::move(callback);
    }

    void set_on_write_complete(WriteCompleteCallback callback) {
        on_write_complete_callback_ = std::move(callback);
    }

    bool enable_tls(const std::string &cert_file, const std::string &key_file,
                    uint32_t handshake_timeout_ms = 10000);

    bool tls_enabled() const { return tls_context_ != nullptr; }

    /**
     * @brief 设置高水位回调。
     * @param callback 跨越高水位阈值时触发。
     * @param high_water_mark 高水位阈值（字节）。
     */
    void set_on_high_water_mark(HighWaterMarkCallback callback,
                                size_t high_water_mark) {
        on_high_water_mark_callback_ = std::move(callback);
        high_water_mark_ = high_water_mark;
    }

    void set_read_timeout(uint32_t timeout_ms) {
        read_timeout_ms_ = timeout_ms;
    }

    void set_write_timeout(uint32_t timeout_ms) {
        write_timeout_ms_ = timeout_ms;
    }

    void set_keepalive_timeout(uint64_t timeout_ms) {
        keepalive_timeout_ms_ = timeout_ms;
    }

    uint32_t read_timeout() const { return read_timeout_ms_; }

    uint32_t write_timeout() const { return write_timeout_ms_; }

    uint64_t keepalive_timeout() const { return keepalive_timeout_ms_; }

    std::shared_ptr<Acceptor> acceptor() const { return acceptor_; }

  private:
    bool do_start();
    void do_stop();

    void handle_connection(Socket::ptr client);
    void remove_connection(int fd);
    void register_connection(const TcpConnection::ptr &connection);

  private:
    std::shared_ptr<Acceptor> acceptor_;
    MessageCallback on_message_callback_;
    ConnectionCallback on_connection_callback_;
    CloseCallback on_close_callback_;
    WriteCompleteCallback on_write_complete_callback_;

    TlsContext::ptr tls_context_;
    uint32_t tls_handshake_timeout_ms_;

    HighWaterMarkCallback on_high_water_mark_callback_;
    size_t high_water_mark_;

    uint32_t read_timeout_ms_;
    uint32_t write_timeout_ms_;
    uint64_t keepalive_timeout_ms_;

    int thread_count_;

    mutable std::mutex connections_mutex_;
    ConnectionMap connections_;

    std::atomic<bool> running_{false};
};

} // namespace znet

#endif // ZNET_TCP_SERVER_H_
