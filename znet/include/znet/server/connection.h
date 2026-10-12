#ifndef ZLYNX_ZNET_INCLUDE_ZNET_SERVER_CONNECTION_H_
#define ZLYNX_ZNET_INCLUDE_ZNET_SERVER_CONNECTION_H_

#include "zco/runtime.h"
#include "zco/sync/mutex.h"
#include "znet/byte_buffer.h"
#include "znet/transport/byte_stream.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

namespace znet {
/**
 * @brief 连接独占传输层；输入缓冲区和协议状态由调用方持有。
 * 普通线程的 I/O 调用提交到注入的执行器并等待完成，
 * 协程调用直接执行；读取与写入分别串行化，两者可以并行。
 */
class Connection {
  public:
    using ptr = std::shared_ptr<Connection>;
    using Timeout = std::chrono::milliseconds;
    enum class State { starting, open, peer_closed, closed };

    Connection(std::unique_ptr<ByteStream> stream, zco::Executor executor,
               Timeout write_timeout = Timeout{0});
    ~Connection();
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    Result<void> start(zco::Deadline deadline = {});
    Transfer read(ByteBuffer &input, size_t limit = 4096,
                  zco::Deadline deadline = {});

    /**
     * @brief 同步写出完整字节串，失败时保留已写入进度。
     * @param timeout
     * 未指定时使用构造时的写超时；零表示无截止时间，负值返回校验错误。
     * @note 写出循环发生错误时关闭传输层；提交或获取写锁失败不保证关闭。
     * bytes 仅在本次调用期间借用；不建立隐藏队列，也不自动重试整个发送操作。
     */
    Transfer send(std::string_view bytes,
                  std::optional<Timeout> timeout = std::nullopt);

    /**
     * @brief 发送 TLS close_notify 或执行 SHUT_WR，然后关闭连接。
     */
    Result<void> shutdown();

    /**
     * @brief 关闭连接并中断读取、写入或握手，包括未设截止时间的等待。
     */
    Result<void> close();

    State state() const { return state_.load(); }

    bool connected() const {
        const auto current = state();
        return current == State::open || current == State::peer_closed;
    }

    int native_handle() const { return stream_->native_handle(); }

    bool encrypted() const { return stream_->encrypted(); }

    Result<Endpoint> local_endpoint() const {
        return stream_->local_endpoint();
    }

    Result<Endpoint> remote_endpoint() const {
        return stream_->remote_endpoint();
    }

    void set_read_deadline(zco::Deadline deadline);
    bool read_deadline_expired() const;

  private:
    std::unique_ptr<ByteStream> stream_;
    zco::Executor executor_;
    const Timeout write_timeout_;
    std::atomic<State> state_{State::starting};
    zco::Mutex reads_, writes_;
    mutable std::mutex deadline_mutex_;
    zco::Deadline read_deadline_;
};
} // namespace znet

#endif
