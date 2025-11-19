#include "looper.h"

namespace zlog {

AsyncLooper::AsyncLooper(Functor func, const AsyncType looperType, const std::chrono::milliseconds milliseco)
    : looperType_(looperType), stop_(false),
      thread_(std::thread(&AsyncLooper::threadEntry, this)), callBack_(std::move(func)), milliseco_(milliseco)
{
}

void AsyncLooper::push(const char *data, const size_t len)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (looperType_ == AsyncType::ASYNC_SAFE)
    {
        condPro_.wait(lock, [&]()
                      { return proBuf_.writeAbleSize() >= len; });
    }
    proBuf_.push(data, len);

    if (proBuf_.readAbleSize() >= FLUSH_BUFFER_SIZE)
        condCon_.notify_one();
}

AsyncLooper::~AsyncLooper()
{
    stop();
}

void AsyncLooper::stop()
{
    stop_ = true;
    condCon_.notify_all();
    thread_.join(); // 等待工作线程退出
}

void AsyncLooper::threadEntry()
{
    while (true)
    {
        {
            // 1.判断生产缓冲区有没有数据
            std::unique_lock<std::mutex> lock(mutex_);

            // 当生产缓冲区为空且标志位被设置的情况下才退出，否则退出时生产缓冲区仍有数据
            if (proBuf_.empty() && stop_ == true)
            {
                break;
            }

            // 等待，超时返回
            if (!condCon_.wait_for(lock, milliseco_, [this]()
                                   { return proBuf_.readAbleSize() >= FLUSH_BUFFER_SIZE || stop_; }))
            {
                if (proBuf_.empty())
                    continue;
            }

            // 2.唤醒后交换缓冲区
            conBuf_.swap(proBuf_);
            if (looperType_ == AsyncType::ASYNC_SAFE)
                condPro_.notify_one();
        }

        // 3.处理数据并初始化
        callBack_(conBuf_);
        conBuf_.reset();
    }
}

} // namespace zlog
