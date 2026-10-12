#include "znet/server/connection.h"
#include "zco/coroutine.h"
#include <mutex>

namespace znet {
namespace {
Transfer failed(std::errc code, const char *operation) {
    return {0, make_error(ErrorKind::runtime, code, operation), false};
}

// 协程路径直接调用，避免类型擦除和堆分配；普通线程调用方仍在
// 借用的操作和缓冲区离开作用域前等待提交任务完成。
template <class Operation>
Transfer perform_io(zco::Executor &executor, Operation &&operation) {
    if (zco::in_coroutine())
        return operation();
    Transfer result;

    // 此处调用方是普通线程，会等待提交任务最终完成，
    // 不会像协程等待者那样因取消而提前返回、使借用缓冲区失效。
    auto task = executor.spawn([&] { result = operation(); });
    if (!task)
        return {0, runtime_error("submit connection IO", task.error()), false};
    auto joined = task.value().join();
    if (!joined)
        return {0, runtime_error("join connection IO", joined.error()), false};
    return result;
}
} // namespace

Connection::Connection(std::unique_ptr<ByteStream> stream,
                       zco::Executor executor, Timeout write_timeout)
    : stream_(std::move(stream)), executor_(std::move(executor)),
      write_timeout_(write_timeout) {
    if (!stream_ || write_timeout.count() < 0)
        throw std::invalid_argument(
            "Connection requires a stream and nonnegative timeout");
}

Connection::~Connection() { (void)close(); }

Result<void> Connection::start(zco::Deadline deadline) {
    auto result = perform_io(executor_, [&]() -> Transfer {
        auto locked = reads_.lock(deadline);
        if (!locked)
            return {0, runtime_error("start connection", locked.error()),
                    false};
        std::lock_guard<zco::Mutex> unlock(reads_, std::adopt_lock);
        if (state() == State::open || state() == State::peer_closed)
            return {};
        if (state() == State::closed)
            return failed(std::errc::bad_file_descriptor, "start connection");
        try {
            auto started = stream_->start(deadline);
            if (!started) {
                (void)close();
                return {0, started.error(), false};
            }
            State expected = State::starting;
            if (!state_.compare_exchange_strong(expected, State::open))
                return failed(std::errc::bad_file_descriptor,
                              "start connection");
            return {};
        } catch (...) {
            (void)close();
            throw;
        }
    });
    if (!result)
        return result.error;
    return {};
}

Transfer Connection::read(ByteBuffer &input, size_t limit,
                          zco::Deadline deadline) {
    if (!limit)
        return {0,
                make_error(ErrorKind::validation, std::errc::invalid_argument,
                           "read limit"),
                false};
    {
        std::lock_guard<std::mutex> lock(deadline_mutex_);
        if (read_deadline_.time() < deadline.time())
            deadline = read_deadline_;
    }
    return perform_io(executor_, [&]() -> Transfer {
        auto locked = reads_.lock(deadline);
        if (!locked)
            return {0, runtime_error("read lock", locked.error()), false};
        std::lock_guard<zco::Mutex> unlock(reads_, std::adopt_lock);
        if (state() == State::peer_closed)
            return {0, {}, true};
        if (state() != State::open)
            return failed(std::errc::bad_file_descriptor, "read connection");
        input.ensure_writable_bytes(limit);
        auto result = stream_->read_some(input.begin_write(), limit, deadline);
        if (result.bytes > limit)
            throw std::logic_error("Transport read exceeds supplied capacity");
        input.has_written(result.bytes);
        if (result.eof) {
            State expected = State::open;
            state_.compare_exchange_strong(expected, State::peer_closed);
        }
        return result;
    });
}

Transfer Connection::send(std::string_view bytes,
                          std::optional<Timeout> timeout) {
    const auto duration = timeout.value_or(write_timeout_);
    if (duration.count() < 0)
        return {0,
                make_error(ErrorKind::validation, std::errc::invalid_argument,
                           "write timeout"),
                false};
    const auto deadline =
        duration.count() ? zco::Deadline::after(duration) : zco::Deadline{};
    return perform_io(executor_, [&]() -> Transfer {
        auto locked = writes_.lock(deadline);
        if (!locked)
            return {0, runtime_error("write lock", locked.error()), false};
        std::lock_guard<zco::Mutex> unlock(writes_, std::adopt_lock);
        if (!connected())
            return failed(std::errc::bad_file_descriptor, "send connection");
        Transfer total;
        try {
            while (total.bytes < bytes.size()) {
                if (deadline.expired(zco::Deadline::Clock::now())) {
                    total.error =
                        make_error(ErrorKind::runtime, std::errc::timed_out,
                                   "send connection");
                    break;
                }
                auto result =
                    stream_->write_some(bytes.data() + total.bytes,
                                        bytes.size() - total.bytes, deadline);
                if (result.bytes > bytes.size() - total.bytes)
                    throw std::logic_error(
                        "Transport write exceeds supplied size");
                total.bytes += result.bytes;
                if (result.error) {
                    total.error = std::move(result.error);
                    break;
                }
                if (!result.bytes || result.eof) {
                    total.error = make_error(
                        ErrorKind::io, std::errc::broken_pipe,
                        "send connection", "transport made no progress");
                    break;
                }
            }
        } catch (...) {
            (void)close();
            throw;
        }
        if (total.error)
            (void)close();
        return total;
    });
}

Result<void> Connection::shutdown() {
    auto result = perform_io(executor_, [&]() -> Transfer {
        // 设置有限等待预算，避免正常 TLS 关闭无限等待。
        const auto deadline = zco::Deadline::after(
            write_timeout_.count() ? write_timeout_ : Timeout{1000});
        auto locked = writes_.lock(deadline);
        if (!locked) {
            (void)close();
            return {0, runtime_error("shutdown lock", locked.error()), false};
        }
        std::lock_guard<zco::Mutex> unlock(writes_, std::adopt_lock);
        if (state() == State::closed)
            return {};
        Result<void> finished;
        try {
            finished = stream_->shutdown_write(deadline);
        } catch (...) {
            (void)close();
            throw;
        }
        auto closed = close();
        if (!finished)
            return {0, finished.error(), false};
        if (!closed)
            return {0, closed.error(), false};
        return {};
    });
    if (!result) {
        (void)close();
        return result.error;
    }
    return {};
}

Result<void> Connection::close() {
    state_.store(State::closed);
    return stream_->close();
}

void Connection::set_read_deadline(zco::Deadline deadline) {
    std::lock_guard<std::mutex> lock(deadline_mutex_);
    read_deadline_ = deadline;
}

bool Connection::read_deadline_expired() const {
    std::lock_guard<std::mutex> lock(deadline_mutex_);
    return read_deadline_.expired(zco::Deadline::Clock::now());
}
} // namespace znet
