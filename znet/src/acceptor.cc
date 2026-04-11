#include "znet/acceptor.h"

#include <cerrno>
#include <cstdint>
#include <utility>

#include "zco/sched.h"
#include "znet/znet_logger.h"

namespace znet {

namespace {

constexpr uint32_t kAcceptErrorBackoffMs = 50;

} // namespace

// 仅保存监听参数，真正的 socket 初始化在 start() 中完成。
Acceptor::Acceptor(Address::ptr listen_address, int backlog)
    : listen_address_(std::move(listen_address)), backlog_(backlog) {}

Acceptor::~Acceptor() { stop(); }

// 启动监听并拉起接收协程。
bool Acceptor::start() {
    bool expected = false;
    // 防止重复启动：只有从 false->true 的调用者会继续执行。
    if (!running_.compare_exchange_strong(expected, true)) {
        ZNET_LOG_DEBUG(
            "Acceptor::start skipped because acceptor is already running");
        return true;
    }

    if (!listen_address_) {
        errno = EINVAL;
        running_.store(false);
        ZNET_LOG_ERROR("Acceptor::start listen address is null");
        return false;
    }

    // 监听 socket 创建失败时立即回滚运行状态。
    listen_socket_ = Socket::create_tcp(listen_address_);
    if (!listen_socket_) {
        running_.store(false);
        ZNET_LOG_ERROR("Acceptor::start failed to create listen socket");
        return false;
    }

    (void)listen_socket_->set_reuse_addr(true);
    (void)listen_socket_->set_reuse_port(true);
    if (!listen_socket_->bind(listen_address_) ||
        !listen_socket_->listen(backlog_)) {
        // bind/listen 任一步失败都需要关闭 fd，避免资源泄漏。
        listen_socket_->close();
        listen_socket_.reset();
        running_.store(false);
        ZNET_LOG_ERROR("Acceptor::start failed during bind/listen");
        return false;
    }

    ZNET_LOG_INFO("Acceptor::start success: addr={}, backlog={}",
                  listen_address_->to_string(), backlog_);

    try {
        // 协程中持有 self，确保 accept_loop 生命周期内对象不被提前释放。
        auto self = shared_from_this();
        zco::go([self]() { self->accept_loop(); });
    } catch (const std::bad_weak_ptr &) {
        ZNET_LOG_ERROR("Acceptor::start must be called on shared_ptr instance");
        listen_socket_->close();
        listen_socket_.reset();
        running_.store(false);
        return false;
    }

    return true;
}

// 停止监听：通过 running_ 让 accept_loop 自然退出，并主动关闭监听 fd。
void Acceptor::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        ZNET_LOG_DEBUG(
            "Acceptor::stop skipped because acceptor is not running");
        return;
    }

    if (listen_socket_) {
        ZNET_LOG_INFO("Acceptor::stop closing listen socket: fd={}",
                      listen_socket_->fd());
        (void)listen_socket_->close();
        listen_socket_.reset();
    }
}

// 主接入循环：处理可重试错误并将成功接入的连接交给上层。
void Acceptor::accept_loop() {
    ZNET_LOG_INFO("Acceptor::accept_loop started");
    while (running_.load()) {
        if (!listen_socket_) {
            ZNET_LOG_WARN(
                "Acceptor::accept_loop exits because listen socket is null");
            break;
        }

        Socket::ptr client = listen_socket_->accept();
        if (!client) {
            if (!running_.load()) {
                ZNET_LOG_DEBUG(
                    "Acceptor::accept_loop exits because acceptor stopped");
                break;
            }

            // 可重试错误由底层 co_accept 等待驱动，这里直接重试即可。
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }

            // 监听 fd 已关闭，通常是 stop() 导致，直接退出循环。
            if (errno == EBADF) {
                ZNET_LOG_DEBUG(
                    "Acceptor::accept_loop exits because listen fd is closed");
                break;
            }

            ZNET_LOG_WARN("Acceptor::accept_loop accept failed: errno={}",
                          errno);
            // 非可重试错误走短暂退避，避免异常场景下 busy loop 与日志风暴。
            zco::sleep_for(kAcceptErrorBackoffMs);
            continue;
        }

        ZNET_LOG_DEBUG("Acceptor::accept_loop accepted client: fd={}",
                       client->fd());

        if (accept_callback_) {
            // 连接交由上层生命周期管理，Acceptor 仅负责分发。
            accept_callback_(std::move(client));
        } else {
            ZNET_LOG_WARN("Acceptor::accept_loop dropped client because "
                          "callback is null");
        }
    }

    ZNET_LOG_INFO("Acceptor::accept_loop stopped");
}

} // namespace znet
