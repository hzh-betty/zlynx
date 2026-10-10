/**
 * @file async_logger.cc
 * @brief 日志器与内部异步执行器的装配。
 */

#include "async/looper.h"
#include "zlog/logger.h"

namespace zlog {

AsyncLogger::AsyncLogger(std::string logger_name, LogLevel::value limit_level,
                         const Formatter::ptr &formatter,
                         const std::vector<LogSink::ptr> &sinks,
                         AsyncType looper_type,
                         std::chrono::milliseconds milliseco)
    : Logger(std::move(logger_name), limit_level, formatter, sinks),
      looper_(new detail::AsyncLooper(
          [this](const char *data, size_t len) { write_sinks(data, len); },
          looper_type, milliseco, [this] { flush_sinks(); })) {}

AsyncLogger::~AsyncLogger() { close_noexcept(); }

void AsyncLogger::log(const char *data, size_t len) { looper_->push(data, len); }

void AsyncLogger::flush() {
    if (looper_->is_worker_thread()) {
        throw std::logic_error("cannot flush an async logger from its own sink");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!closed_) {
        looper_->flush();
    }
}

void AsyncLogger::close() {
    if (looper_->is_worker_thread()) {
        throw std::logic_error("cannot close an async logger from its own sink");
    }
    std::vector<LogSink::ptr> released;
    std::exception_ptr failure;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_.exchange(true)) {
            return;
        }
        try {
            looper_->stop();
        } catch (...) {
            failure = std::current_exception();
        }
        sinks_.swap(released);
    }
    released.clear();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

} // namespace zlog
