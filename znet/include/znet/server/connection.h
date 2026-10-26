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

    /*
    *   Connection::State 状态转换图（连接生命周期）
    *
    *                          构造
    *                           |
    *                           v
    *                     +------------+
    *                     |  starting  |-------------------+
    *                     +------------+                   |
    *                           |                          |
    *                    start 启动成功              启动失败 / 异常
    *                           |                          |
    *                           v                          |
    *                     +------------+                   |
    *                     |    open    |-------------------+
    *                     +------------+                   |
    *                           |                          |
    *                      read 收到 EOF              send 写出失败
    *                           |                          |
    *                           v                          |
    *                     +-------------+                  |
    *                     | peer_closed |------------------+
    *                     +-------------+                  |
    *                                                      v
    *                                                +----------+
    *   任意未关闭状态 -- close / shutdown / 析构 --> |  closed  |
    *                                                +----------+
    *
    * 状态说明：
    *   starting    : 已构造，尚未完成传输层启动或 TLS 握手
    *   open        : 启动成功，允许读取和发送
    *   peer_closed : 已收到对端 EOF，读取直接返回 EOF，仍允许发送
    *   closed      : 已关闭，不可重新启动
    *
    * 常规转换：
    *   构造                  -> starting
    *   starting -> open        : start 中底层启动或握手成功
    *   open     -> peer_closed : read 中底层返回 EOF
    *   任意状态 -> closed      : close，或析构时调用 close
    *   任意状态 -> closed      : shutdown 尝试关闭写端后关闭连接；
    *                             shutdown 失败也会调用 close
    *
    * 错误路径：
    *   starting    -> closed : 底层 start 失败或抛异常
    *   open        -> closed : send 写出循环失败或抛异常
    *   peer_closed -> closed : send 写出循环失败或抛异常
    *
    * 保持状态的路径：
    *   open / peer_closed : 重复 start 直接返回成功
    *   peer_closed        : 后续 read 直接返回 EOF
    *   open               : read 普通错误或超时，且未返回 EOF
    *   open / peer_closed : send 成功
    *   closed             : start / read / send 返回错误
    *
    * 注意：
    *   connected() 在 open 和 peer_closed 时均为 true。
    *   send 写出失败包括写超时、底层错误、零进展或写端 EOF。
    *   参数校验、任务提交或 start/read/send 获取锁失败，
    *   本身不触发状态转换。
    *   shutdown 不保留“本端写半关闭”状态，最终关闭整个连接。
    *   close 先设置 closed，再关闭底层传输；底层关闭报错也不回退。
    *   状态转换使用原子操作，并发 close 后不会重新变为 open
    *   或 peer_closed。
    */
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

    /**
     * @brief 检查 read_deadline_ 是否过期；在调用 read 前检查，避免重复获取锁。
     */
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
