#include "znet/tcp_server.h"

#include <atomic>
#include <cerrno>
#include <string>
#include <utility>

#include "zcoroutine/sched.h"
#include "znet/socket.h"
#include "znet/znet_logger.h"

namespace znet {

namespace {
bool is_retryable_read_errno(int err) {
    return err == EINTR || err == EAGAIN || err == EWOULDBLOCK;
}

bool is_peer_disconnect_errno(int err) {
    return err == ECONNRESET || err == ENOTCONN || err == EPIPE;
}

} // namespace

TcpServer::TcpServer(Address::ptr listen_address, int backlog)
    : acceptor_(std::make_shared<Acceptor>(std::move(listen_address), backlog)),
      on_message_callback_(), on_connection_callback_(), on_close_callback_(),
      on_write_complete_callback_(), tls_context_(),
      tls_handshake_timeout_ms_(10000), on_high_water_mark_callback_(),
      high_water_mark_(64 * 1024 * 1024), read_timeout_ms_(100),
      write_timeout_ms_(0), keepalive_timeout_ms_(0), thread_count_(0),
      connections_mutex_(), connections_(), running_(false) {}

bool TcpServer::enable_tls(const std::string &cert_file,
                           const std::string &key_file,
                           uint32_t handshake_timeout_ms) {
    std::string error;
    auto tls_context =
        create_server_tls_context_openssl(cert_file, key_file, &error);
    if (!tls_context) {
        ZNET_LOG_ERROR(
            "TcpServer::enable_tls failed: cert={}, key={}, error={}",
            cert_file, key_file, error);
        return false;
    }

    tls_context_ = std::move(tls_context);
    tls_handshake_timeout_ms_ = handshake_timeout_ms;

    ZNET_LOG_INFO("TcpServer::enable_tls enabled: cert={}, key={}", cert_file,
                  key_file);
    return true;
}

bool TcpServer::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true,
                                          std::memory_order_acq_rel)) {
        ZNET_LOG_DEBUG(
            "TcpServer::start skipped because server is already running");
        return true;
    }

    if (!do_start()) {
        running_.store(false, std::memory_order_release);
        return false;
    }

    return true;
}

void TcpServer::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false,
                                          std::memory_order_acq_rel)) {
        ZNET_LOG_DEBUG("TcpServer::stop skipped because server is not running");
        return;
    }

    do_stop();
}

bool TcpServer::do_start() {
    if (!acceptor_) {
        ZNET_LOG_ERROR("TcpServer::do_start failed because acceptor is null");
        return false;
    }

    zcoroutine::init(thread_count_);
    ZNET_LOG_INFO(
        "TcpServer::do_start initialized zcoroutine runtime: thread_count={}",
        thread_count_);

    acceptor_->set_accept_callback(
        [this](Socket::ptr client) { handle_connection(std::move(client)); });

    const bool ok = acceptor_->start();
    if (ok) {
        ZNET_LOG_INFO("TcpServer::do_start acceptor started successfully");
    } else {
        ZNET_LOG_ERROR("TcpServer::do_start failed to start acceptor");
    }
    return ok;
}

void TcpServer::do_stop() {
    ZNET_LOG_INFO("TcpServer::do_stop begin");
    if (acceptor_) {
        acceptor_->stop();
    }

    auto close_all = [this]() {
        ConnectionMap snapshot;
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            snapshot.swap(connections_);
        }
        for (auto &item : snapshot) {
            if (item.second) {
                item.second->close();
            }
        }
        ZNET_LOG_INFO("TcpServer::do_stop closed all connections: count={}",
                      snapshot.size());
    };

    close_all();

    ZNET_LOG_INFO("TcpServer::do_stop completed");
}

void TcpServer::handle_connection(Socket::ptr client) {
    if (!client) {
        ZNET_LOG_WARN(
            "TcpServer::handle_connection ignored null client socket");
        return;
    }

    zcoroutine::Scheduler *scheduler = zcoroutine::next_sched();

    ZNET_LOG_DEBUG(
        "TcpServer::handle_connection dispatch: client_fd={}, sched_id={}",
        client->fd(), scheduler ? scheduler->id() : -1);

    std::shared_ptr<TcpServer> self = shared_from_this();
    auto run_connection = [self, scheduler,
                           client = std::move(client)]() mutable {
        ZNET_LOG_INFO("TcpServer::handle_connection begin: client_fd={}",
                      client->fd());

        TcpConnection::ptr connection =
            std::make_shared<TcpConnection>(client, scheduler);
        connection->set_write_timeout(self->write_timeout_ms_);
        if (self->on_write_complete_callback_) {
            connection->set_write_complete_callback(
                self->on_write_complete_callback_);
        }
        if (self->on_high_water_mark_callback_) {
            connection->set_high_water_mark_callback(
                self->on_high_water_mark_callback_, self->high_water_mark_);
        }

        if (self->tls_context_) {
            if (!connection->enable_tls_server(
                    self->tls_context_, self->tls_handshake_timeout_ms_)) {
                ZNET_LOG_WARN(
                    "TcpServer::handle_connection TLS handshake failed: fd={}",
                    connection->fd());
                connection->close();
                return;
            }
        }

        self->register_connection(connection);

        if (self->on_connection_callback_) {
            self->on_connection_callback_(connection);
        }

        uint64_t idle_elapsed_ms = 0;
        while (self->is_running() && connection->connected()) {
            const uint32_t read_timeout_ms = self->read_timeout_ms_;
            const ssize_t n = connection->read(4096, read_timeout_ms);
            if (n > 0) {
                idle_elapsed_ms = 0;
                if (self->on_message_callback_) {
                    self->on_message_callback_(connection,
                                               connection->input_buffer());
                }
                continue;
            }

            if (n == 0) {
                ZNET_LOG_INFO("TcpServer::handle_connection peer closed: fd={}",
                              connection->fd());
                break;
            }

            const int read_err = errno;
            if (read_err == ETIMEDOUT) {
                if (self->keepalive_timeout_ms_ > 0 && read_timeout_ms > 0) {
                    idle_elapsed_ms += read_timeout_ms;
                    if (idle_elapsed_ms >= self->keepalive_timeout_ms_) {
                        ZNET_LOG_INFO(
                            "TcpServer::handle_connection keepalive timeout: "
                            "fd={}, keepalive_timeout_ms={}",
                            connection->fd(), self->keepalive_timeout_ms_);
                        break;
                    }
                }
                continue;
            }

            if (is_retryable_read_errno(read_err)) {
                continue;
            }

            if (is_peer_disconnect_errno(read_err) || read_err == EBADF) {
                ZNET_LOG_INFO(
                    "TcpServer::handle_connection disconnected by peer/error: "
                    "fd={}, errno={}",
                    connection->fd(), read_err);
                break;
            }

            ZNET_LOG_WARN(
                "TcpServer::handle_connection keep alive on read error: "
                "fd={}, errno={}",
                connection->fd(), read_err);
        }

        if (self->on_close_callback_) {
            self->on_close_callback_(connection);
        }

        connection->close();
        self->remove_connection(connection->fd());
        ZNET_LOG_INFO("TcpServer::handle_connection end: fd={}",
                      connection->fd());
    };

    if (!scheduler) {
        run_connection();
        return;
    }

    scheduler->go(std::move(run_connection));
}

void TcpServer::register_connection(const TcpConnection::ptr &connection) {
    if (!connection) {
        ZNET_LOG_WARN("TcpServer::register_connection ignored null connection");
        return;
    }

    const int fd = connection->fd();
    size_t total = 0;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_[fd] = connection;
        total = connections_.size();
    }
    ZNET_LOG_DEBUG("TcpServer::register_connection success: fd={}, total={}",
                   fd, total);
}

void TcpServer::remove_connection(int fd) {
    size_t total = 0;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_.erase(fd);
        total = connections_.size();
    }
    ZNET_LOG_DEBUG("TcpServer::remove_connection success: fd={}, total={}", fd,
                   total);
}

} // namespace znet
