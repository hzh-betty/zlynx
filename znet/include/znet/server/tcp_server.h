#pragma once

#include "znet/server/connection.h"
#include "znet/transport/tls_credentials.h"
#include <functional>

namespace znet {
// Created once per established connection. Captured protocol state belongs to
// these callbacks, not to the network connection. Both run on its session task.
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
    // One terminal error report at the service boundary. Never called under a
    // server lock; must not throw. Expected EOF/stop/read timeouts are omitted.
    std::function<void(const Error &)> on_error;
};

// Runtime is borrowed and must outlive this server. Stack/unique_ptr ownership
// is sufficient. Configuration is immutable. start/stop are control-thread
// operations; callbacks use request_stop to avoid joining themselves.
class TcpServer {
  public:
    TcpServer(zco::Runtime &runtime, Endpoint endpoint, SessionFactory factory,
              ServerOptions options = {});
    ~TcpServer();
    TcpServer(const TcpServer &) = delete;
    TcpServer &operator=(const TcpServer &) = delete;

    Result<void> start();
    void request_stop();
    void stop(); // Waits for every accept/session task and its callbacks.
    bool is_running() const;
    Result<Endpoint> local_endpoint() const;
    size_t active_connections() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace znet
