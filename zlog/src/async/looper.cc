/**
 * @file looper.cc
 * @brief 异步日志的记录队列与完成屏障。
 * @author hzh-betty
 */

#include "looper.h"
#include "zlog/logger.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>

namespace zlog {
namespace detail {

void *AsyncLooper::operator new(size_t size) {
    void *memory = nullptr;
    if (posix_memalign(&memory, alignof(AsyncLooper), size) != 0) {
        throw std::bad_alloc();
    }
    return memory;
}

void AsyncLooper::operator delete(void *memory) noexcept { std::free(memory); }

AsyncLooper::AsyncLooper(Functor func, AsyncType looper_type,
                         std::chrono::milliseconds milliseco, FlushFunctor flush)
    : looper_type_(looper_type), callback_(std::move(func)),
      flush_callback_(std::move(flush)), milliseco_(milliseco) {
    if (milliseco_.count() <= 0) {
        throw std::invalid_argument("async wait time must be positive");
    }
    thread_ = std::thread(&AsyncLooper::thread_entry, this);
    worker_id_ = thread_.get_id();
}

bool AsyncLooper::is_worker_thread() const {
    return std::this_thread::get_id() == worker_id_;
}

void AsyncLooper::push(const char *data, size_t len) {
    if (is_worker_thread()) {
        throw std::logic_error("cannot log to an async logger from its own sink");
    }
    const size_t limit = looper_type_ == AsyncType::ASYNC_SAFE
                             ? kDefaultBufferSize : kMaxBufferSize;
    if (len > limit - kRecordHeaderSize) {
        throw std::length_error("log message exceeds async buffer capacity");
    }
    if (len != 0 && !data) {
        throw std::invalid_argument("null async log data");
    }
    const size_t needed = kRecordHeaderSize + len;
    std::unique_lock<Spinlock> lock(mutex_);
    cond_pro_.wait(lock, [&] {
        return stop_ || (looper_type_ == AsyncType::ASYNC_SAFE
                            ? pro_buf_.writable_size() >= needed
                            : pro_buf_.can_accommodate(needed));
    });
    if (stop_) {
        throw std::runtime_error("async logger is closed");
    }
    // 扩容完成后才写入长度头，保证分配失败不会留下半条记录。
    pro_buf_.reserve(needed);
    pro_buf_.push(reinterpret_cast<const char *>(&len), kRecordHeaderSize);
    pro_buf_.push(data, len);
    ++accepted_;
    if (pro_buf_.readable_size() >= kFlushBufferSize) {
        cond_con_.notify_one();
    }
}

void AsyncLooper::flush() {
    if (is_worker_thread()) {
        throw std::logic_error("cannot flush an async logger from its own sink");
    }
    std::lock_guard<std::mutex> operation(stop_mutex_);
    std::unique_lock<Spinlock> lock(mutex_);
    if (!stop_) {
        const uint64_t request = ++flush_requested_;
        flush_target_ = accepted_;
        cond_con_.notify_one();
        cond_done_.wait(lock, [&] { return flush_completed_ >= request; });
    }
    if (callback_exception_) {
        std::rethrow_exception(callback_exception_);
    }
}

void AsyncLooper::stop() {
    if (is_worker_thread()) {
        throw std::logic_error("cannot close an async logger from its own sink");
    }
    std::lock_guard<std::mutex> operation(stop_mutex_);
    {
        std::unique_lock<Spinlock> lock(mutex_);
        stop_ = true;
    }
    cond_pro_.notify_all();
    cond_con_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    if (callback_exception_) {
        std::rethrow_exception(callback_exception_);
    }
}

AsyncLooper::~AsyncLooper() noexcept {
    try {
        stop();
    } catch (...) {
    }
}

void AsyncLooper::record_exception(std::exception_ptr exception) {
    std::unique_lock<Spinlock> lock(mutex_);
    if (!callback_exception_) {
        callback_exception_ = exception;
    }
}

void AsyncLooper::thread_entry() {
    for (;;) {
        {
            std::unique_lock<Spinlock> lock(mutex_);
            cond_con_.wait_for(lock, milliseco_, [&] {
                return stop_ || flush_requested_ != flush_completed_ ||
                       pro_buf_.readable_size() >= kFlushBufferSize;
            });
            con_buf_.swap(pro_buf_);
            cond_pro_.notify_all();
        }

        uint64_t records = 0;
        size_t offset = 0;
        while (offset < con_buf_.readable_size()) {
            size_t len;
            std::memcpy(&len, con_buf_.begin() + offset, kRecordHeaderSize);
            offset += kRecordHeaderSize;
            try {
                callback_(con_buf_.begin() + offset, len);
            } catch (...) {
                record_exception(std::current_exception());
            }
            offset += len;
            ++records;
        }
        con_buf_.reset();

        uint64_t request = 0;
        bool stopping = false;
        bool flushing = false;
        {
            std::unique_lock<Spinlock> lock(mutex_);
            completed_ += records;
            if (completed_ >= flush_target_) {
                request = flush_requested_;
            }
            stopping = stop_ && pro_buf_.empty();
            flushing = request != flush_completed_ || stopping;
        }
        if (flushing) {
            try {
                if (flush_callback_) {
                    flush_callback_();
                }
            } catch (...) {
                record_exception(std::current_exception());
            }
            {
                std::unique_lock<Spinlock> lock(mutex_);
                flush_completed_ = request;
            }
            cond_done_.notify_all();
        }
        if (stopping) {
            return;
        }
    }
}

} // namespace detail
} // namespace zlog
