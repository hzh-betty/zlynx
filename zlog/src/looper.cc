#include "looper.h"

#include <iostream>

namespace zlog {

AsyncLooper::AsyncLooper(Functor func, const AsyncType looper_type,
                         const std::chrono::milliseconds milliseco)
    : looper_type_(looper_type), stop_(false),
      thread_(std::thread(&AsyncLooper::thread_entry, this)),
      callback_(std::move(func)), milliseco_(milliseco) {}

void AsyncLooper::push(const char *data, const size_t len) {
    std::unique_lock<Spinlock> lock(mutex_);
    if (looper_type_ == AsyncType::ASYNC_SAFE) {
        // 安全模式下等待缓冲区有空闲空间
        cond_pro_.wait(lock, [&]() { return pro_buf_.writable_size() >= len; });
    } else {
        // UNSAFE模式下，如果扩容会超过最大缓冲区大小，则阻塞等待
        cond_pro_.wait(lock, [&]() { return pro_buf_.can_accommodate(len); });
    }

    pro_buf_.push(data, len); // 向缓冲区推送数据

    if (pro_buf_.readable_size() >=
        kFlushBufferSize) { // 缓冲区可读空间大于阈值
        cond_con_.notify_one();
    }
}

AsyncLooper::~AsyncLooper() { stop(); }

void AsyncLooper::stop() {
    stop_ = true;
    cond_con_.notify_all();
    if (thread_.joinable()) {
        thread_.join(); // 等待工作线程退出
    }
}

void AsyncLooper::thread_entry() {
    while (true) {
        {
            // 1. 等待条件满足
            std::unique_lock<Spinlock> lock(mutex_);

            // 检查是否需要退出或有数据
            if (pro_buf_.empty() && stop_) {
                break;
            }

            // 等待，超时返回
            cond_con_.wait_for(lock, milliseco_, [this]() {
                return (pro_buf_.readable_size() >= kFlushBufferSize) || stop_;
            });

            // 2. 交换缓冲区
            if (pro_buf_.empty()) {
                if (stop_)
                    break;
                continue; // 虚假唤醒或超时但无数据
            }
            con_buf_.swap(pro_buf_);

            // 3. 唤醒生产者
            cond_pro_.notify_one();
        }

        // 4.处理数据并初始化
        try {
            callback_(con_buf_);
        } catch (const std::exception &e) {
            std::cerr << "AsyncLooper callback exception: " << e.what()
                      << std::endl;
        } catch (...) {
            std::cerr << "AsyncLooper callback unknown exception" << std::endl;
        }
        con_buf_.reset();
    }
}

} // namespace zlog
