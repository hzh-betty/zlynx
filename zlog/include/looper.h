#ifndef ZLOG_LOOPER_H_
#define ZLOG_LOOPER_H_
#include "buffer.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <memory>
#include <atomic>
#include <chrono>

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
        AsyncLooper(Functor func, AsyncType looperType, std::chrono::milliseconds milliseco);

        /**
         * @brief 向生产缓冲区推送数据
         * @param data 数据指针
         * @param len 数据长度
         */
        void push(const char *data, size_t len);

        /**
         * @brief 析构函数
         * 停止异步循环器并等待工作线程结束
         */
        ~AsyncLooper();

        /**
         * @brief 停止异步循环器
         */
        void stop();

    private:
        /**
         * @brief 工作线程入口函数
         * 处理消费缓冲区中的数据，处理完毕后初始化缓冲区并交换缓冲区
         */
        void threadEntry();

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

#endif // ZLOG_LOOPER_H_
