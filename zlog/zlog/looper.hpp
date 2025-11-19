#pragma once
#include "buffer.hpp"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <memory>
#include <atomic>
#include <chrono>
#include <utility>

namespace zlog
{
    /**
     * @brief 异步日志器类型枚举
     */
    enum class AsyncType
    {
        ASYNC_SAFE,     ///< 固定长度的缓冲区--阻塞模式
        ASYNC_UNSAFE    ///< 可扩容缓冲区--非阻塞模式
    };

    static constexpr size_t FLUSH_BUFFER_SIZE = DEFAULT_BUFFER_SIZE / 2; ///< 刷新缓冲区大小阈值

    /**
     * @brief 异步日志循环器
     * 实现生产者-消费者模式的异步日志处理
     */
    class AsyncLooper
    {
    public:
        using Functor = std::function<void(Buffer &)>;     ///< 回调函数类型
        using ptr = std::shared_ptr<AsyncLooper>;          ///< 智能指针类型

        /**
         * @brief 构造函数
         * @param func 日志处理回调函数
         * @param looperType 异步类型（安全/非安全）
         * @param milliseco 最大等待时间（毫秒）
         */
        AsyncLooper(Functor func, const AsyncType looperType, const std::chrono::milliseconds milliseco)
            : looperType_(looperType), stop_(false),
              thread_(std::thread(&AsyncLooper::threadEntry, this)), callBack_(std::move(func)), milliseco_(milliseco)
        {
        }

        /**
         * @brief 向生产缓冲区推送数据
         * @param data 数据指针
         * @param len 数据长度
         */
        void push(const char *data, const size_t len)
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

        /**
         * @brief 析构函数
         * 停止异步循环器并等待工作线程结束
         */
        ~AsyncLooper()
        {
            stop();
        }

        /**
         * @brief 停止异步循环器
         */
        void stop()
        {
            stop_ = true;
            condCon_.notify_all();
            thread_.join(); // 等待工作线程退出
        }

    private:
        /**
         * @brief 工作线程入口函数
         * 处理消费缓冲区中的数据，处理完毕后初始化缓冲区并交换缓冲区
         */
        void threadEntry()
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

    private:
        AsyncType looperType_;                          ///< 异步类型
        std::atomic<bool> stop_;                        ///< 停止标志
        Buffer proBuf_;                                 ///< 生产缓冲区
        Buffer conBuf_;                                 ///< 消费缓冲区
        std::mutex mutex_;                              ///< 互斥锁
        std::condition_variable condPro_;               ///< 生产者条件变量
        std::condition_variable condCon_;               ///< 消费者条件变量
        std::thread thread_;                            ///< 工作线程
        Functor callBack_;                              ///< 回调函数
        std::chrono::milliseconds milliseco_;           ///< 最大等待时间
    };
} // namespace zlog