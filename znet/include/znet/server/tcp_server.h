#ifndef ZLYNX_ZNET_INCLUDE_ZNET_SERVER_TCP_SERVER_H_
#define ZLYNX_ZNET_INCLUDE_ZNET_SERVER_TCP_SERVER_H_

#include "znet/server/connection.h"
#include "znet/transport/tls_credentials.h"
#include <functional>

namespace znet {
/**
 * @brief 为每条建立的连接创建一次；捕获的协议状态由回调持有，
 * 不属于网络连接对象；两个回调均在该连接的会话任务中运行。
 */
struct SessionCallbacks {
    std::function<void(const Connection::ptr &, ByteBuffer &)> on_message;
    std::function<void(const Connection::ptr &)> on_close;
};

using SessionFactory = std::function<SessionCallbacks(const Connection::ptr &)>;

struct ServerOptions {
    int backlog = SOMAXCONN;
    size_t read_chunk_size = 4096;
    std::chrono::milliseconds read_timeout{0};
    std::chrono::milliseconds write_timeout{0};
    std::chrono::milliseconds idle_timeout{0};
    std::chrono::milliseconds handshake_timeout{10000};
    bool reuse_port = false;
    std::shared_ptr<const TlsCredentials> tls;

    /**
     * @brief 在服务边界报告错误，不持有服务器锁；回调不应抛出异常。
     * 预期的 EOF、停止和读取超时不报告；会话处理与关闭回调的错误可分别报告。
     * @note 不同任务可能并发调用；Error 引用仅在回调期间有效。
     */
    std::function<void(const Error &)> on_error;
};

/**
 * @brief 借用的 Runtime 必须比服务器存活更久；服务器可在栈上或由 unique_ptr
 * 持有。 配置在构造后不再修改；start/stop 由非协程的控制线程调用， 回调应使用
 * request_stop，避免等待包含自身的任务组。
 */
class TcpServer {
  public:
    TcpServer(zco::Runtime &runtime, Endpoint endpoint, SessionFactory factory,
              ServerOptions options = {});
    ~TcpServer();
    TcpServer(const TcpServer &) = delete;
    TcpServer &operator=(const TcpServer &) = delete;
    
    /*
      TcpServer::start()
          │
          └─ 创建本轮 ServerRun
                │
                ├─ 启动 reaper 任务：回收已完成的会话任务
                │
                └─ 启动 accept 任务：循环接收连接
                      │
                      └─ SessionManager::admit()
                              ├─ 创建 Connection
                              ├─ 登记会话
                              ├─ 选择 worker
                              └─ 提交会话任务
                                    │
                                    ├─ serve_session(...)
                                    │    ├─ 启动传输 / TLS 握手
                                    │    ├─ factory 创建业务回调
                                    │    ├─ 循环读取并执行 on_message
                                    │    ├─ 关闭连接
                                    │    └─ 执行 on_close
                                    │
                                    └─ session_completed(id)    
    */
    Result<void> start();
    
    void request_stop();

    /**
     * @brief 请求停止并等待全部接收连接任务、会话任务及其回调完成。
     * @throws std::logic_error 从协程中调用；回调应改用 request_stop()。
     */
    void stop();
    bool is_running() const;
    Result<Endpoint> local_endpoint() const;
    size_t active_connections() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace znet

#endif
