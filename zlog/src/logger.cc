/**
 * @file logger.cc
 * @brief logger 的同步输出、刷新与关闭。
 * @author hzh-betty
 */

#include "zlog/logger.h"

#include <cstdio>
#include <exception>
#include <stdexcept>

namespace zlog {
namespace {
// 不分配内存的调用栈，拒绝同一 logger 在 sink 回调中的递归操作。
struct SinkCall {
    explicit SinkCall(const Logger *logger) : logger(logger), previous(active) {
        active = this;
    }
    ~SinkCall() { active = previous; }
    const Logger *logger;
    SinkCall *previous;
    static thread_local SinkCall *active;
};
thread_local SinkCall *SinkCall::active = nullptr;
} // namespace

Logger::Logger(std::string logger_name, LogLevel::value limit_level,
               Formatter::ptr formatter, const std::vector<LogSink::ptr> &sinks)
    : logger_name_(std::move(logger_name)), limit_level_(limit_level),
      formatter_(std::move(formatter)), sinks_(sinks) {
    if (!formatter_) {
        throw std::invalid_argument("logger formatter is null");
    }
    for (const auto &sink : sinks_) {
        if (!sink) {
            throw std::invalid_argument("logger sink is null");
        }
    }
}

void Logger::serialize(LogLevel::value level, const char *file, size_t line,
                       fmt::string_view data) {
    // 1. 日志消息由本次调用持有，正文视图借用本次调用独占的缓冲区。
    const LogMessage msg(level, file, line, data,
                         fmt::string_view(logger_name_.data(), logger_name_.size()));
    // 2. 线程局部序列化缓冲区复用容量，直到所有 sink 返回前都保持占用。
    thread_local fmt::memory_buffer buffer;
    thread_local bool buffer_in_use = false;
    const auto write = [&](fmt::memory_buffer &output) {
        output.clear();
        formatter_->format(output, msg);
        // 3. 输出日志；同步 sink 返回或异步队列完成复制后才归还缓冲区。
        log(output.data(), output.size());
    };
    if (buffer_in_use) {
        // 跨 logger 嵌套输出使用临时缓冲区，不能清空或扩容外层正在使用的缓存。
        fmt::memory_buffer nested_buffer;
        write(nested_buffer);
    } else {
        BufferUse use(buffer_in_use);
        write(buffer);
    }
}

bool Logger::is_active_on_current_thread() const {
    for (SinkCall *call = SinkCall::active; call; call = call->previous) {
        if (call->logger == this) {
            return true;
        }
    }
    return false;
}

void Logger::write_sinks(const char *data, size_t len) {
    SinkCall call(this);
    std::exception_ptr failure;
    for (const auto &sink : sinks_) {
        try {
            sink->log(data, len);
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void Logger::flush_sinks() {
    SinkCall call(this);
    std::exception_ptr failure;
    for (const auto &sink : sinks_) {
        try {
            sink->flush();
        } catch (...) {
            if (!failure) {
                failure = std::current_exception();
            }
        }
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
}

void Logger::close_noexcept() noexcept {
    try {
        close();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "zlog: closing logger '%s' failed: %s\n",
                     logger_name_.c_str(), error.what());
    } catch (...) {
        std::fprintf(stderr, "zlog: closing logger '%s' failed\n", logger_name_.c_str());
    }
}

SyncLogger::SyncLogger(std::string logger_name, LogLevel::value limit_level,
                       const Formatter::ptr &formatter,
                       const std::vector<LogSink::ptr> &sinks)
    : Logger(std::move(logger_name), limit_level, formatter, sinks) {}

SyncLogger::~SyncLogger() { close_noexcept(); }

void SyncLogger::log(const char *data, size_t len) {
    if (is_active_on_current_thread()) {
        throw std::logic_error("cannot log to a sync logger from its own sink");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
        throw std::runtime_error("logger is closed");
    }
    write_sinks(data, len);
}

void SyncLogger::flush() {
    if (is_active_on_current_thread()) {
        throw std::logic_error("cannot flush a sync logger from its own sink");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!closed_) {
        flush_sinks();
    }
}

void SyncLogger::close() {
    if (is_active_on_current_thread()) {
        throw std::logic_error("cannot close a sync logger from its own sink");
    }
    std::vector<LogSink::ptr> released;
    std::exception_ptr failure;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_.exchange(true)) {
            return;
        }
        try {
            flush_sinks();
        } catch (...) {
            failure = std::current_exception();
        }
        sinks_.swap(released);
    }
    // sink 可能被其他 logger 共享；只释放本 logger 的所有权。
    released.clear();
    if (failure) {
        std::rethrow_exception(failure);
    }
}

} // namespace zlog
