#pragma once

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
// A connection owns one transport. Input/protocol state belongs to the caller.
// IO calls from ordinary threads use the injected executor and wait to finish;
// coroutine callers execute directly. Reads and writes serialize independently.
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
    // Success means all bytes sent. On IO failure the transport is closed;
    // result.bytes still reports progress. No hidden queue or implicit retry.
    Transfer send(std::string_view bytes,
                  std::optional<Timeout> timeout = std::nullopt);
    Result<void> shutdown(); // Sends TLS close_notify/SHUT_WR, then closes.
    Result<void> close(); // Interrupts even an infinite read/write/handshake.

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
