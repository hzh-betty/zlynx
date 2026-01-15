#include "looper.h"

#include <iostream>

namespace zlog {

AsyncLooper::AsyncLooper(Functor func, const AsyncType looperType,
                         const std::chrono::milliseconds milliseco)
    : looperType_(looperType), stop_(false),
      thread_(std::thread(&AsyncLooper::threadEntry, this)),
      callBack_(std::move(func)), milliseco_(milliseco) {}

void AsyncLooper::push(const char *data, const size_t len) {
  std::unique_lock<Spinlock> lock(mutex_);
  if (looperType_ == AsyncType::ASYNC_SAFE) {
    // 安全模式下等待缓冲区有空闲空间
    condPro_.wait(lock, [&]() { return proBuf_.writeAbleSize() >= len; });
  } else {
    // UNSAFE模式下，如果扩容会超过最大缓冲区大小，则阻塞等待
    condPro_.wait(lock, [&]() { return proBuf_.canAccommodate(len); });
  }

  proBuf_.push(data, len); // 向缓冲区推送数据

  if (proBuf_.readAbleSize() >= FLUSH_BUFFER_SIZE) { // 缓冲区可读空间大于阈值
    condCon_.notify_one();
  }
}

AsyncLooper::~AsyncLooper() { stop(); }

void AsyncLooper::stop() {
  stop_ = true;
  condCon_.notify_all();
  if (thread_.joinable()) {
    thread_.join(); // 等待工作线程退出
  }
}

void AsyncLooper::threadEntry() {
  while (true) {
    {
      // 1. 等待条件满足
      std::unique_lock<Spinlock> lock(mutex_);

      // 检查是否需要退出或有数据
      if (proBuf_.empty() && stop_) {
        break;
      }

      // 等待，超时返回
      condCon_.wait_for(lock, milliseco_, [this]() {
        return (proBuf_.readAbleSize() >= FLUSH_BUFFER_SIZE) || stop_;
      });

      // 2. 交换缓冲区
      if (proBuf_.empty()) {
        if (stop_)
          break;
        continue; // 虚假唤醒或超时但无数据
      }
      conBuf_.swap(proBuf_);

      // 3. 唤醒生产者
      condPro_.notify_one();
    }

    // 4.处理数据并初始化
    try {
      callBack_(conBuf_);
    } catch (const std::exception &e) {
      std::cerr << "AsyncLooper callback exception: " << e.what() << std::endl;
    } catch (...) {
      std::cerr << "AsyncLooper callback unknown exception" << std::endl;
    }
    conBuf_.reset();
  }
}

} // namespace zlog
