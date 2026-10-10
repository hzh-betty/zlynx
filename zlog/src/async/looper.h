/**
 * @file looper.h
 * @brief 异步日志的背压、完成与关闭。
 * @author hzh-betty
 */

#ifndef ZLOG_INTERNAL_LOOPER_H_
#define ZLOG_INTERNAL_LOOPER_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

#include "buffer.h"

namespace zlog {
enum class AsyncType;
namespace detail {
static constexpr size_t kFlushBufferSize = kDefaultBufferSize / 32; // 刷新缓冲区大小阈值
// 长度头计入容量；零长度日志也占用空间并保留记录边界。
static constexpr size_t kRecordHeaderSize = sizeof(size_t);

/**
 * @brief 异步日志循环器
 * 实现生产者-消费者模式的异步日志处理；双缓冲交换保留批量收取和单条输出。
 */
class AsyncLooper {
  public:
    using Functor = std::function<void(const char *, size_t)>; // 单条日志回调函数类型
    using FlushFunctor = std::function<void()>;

    /**
     * @brief 构造函数
     * @param func 日志处理回调函数
     * @param looper_type 异步类型（固定容量/允许扩容）
     * @param milliseco 最大等待时间（毫秒），必须大于零
     * @param flush 输出完成后的 sink 刷新回调
     */
    AsyncLooper(Functor func, AsyncType looper_type,
                std::chrono::milliseconds milliseco, FlushFunctor flush = {});
    // C++11 的普通 new 不保证过度对齐，为内部缓存行对齐提供匹配的分配与释放。
    static void *operator new(size_t size);
    static void operator delete(void *memory) noexcept;

    /**
     * @brief 向生产缓冲区推送一条自有记录；满时等待。
     * @param data 数据指针
     * @param len 数据长度
     * @throws std::length_error 单条日志与长度头超过模式允许的容量。
     * @throws std::runtime_error 循环器已停止，包括等待期间停止。
     */
    void push(const char *data, size_t len);
    // 等待本次调用前已接收的记录完成输出和 sink 刷新。
    void flush();
    /** @brief 唤醒等待者，排空队列，刷新并等待工作线程结束。 */
    void stop();
    bool is_worker_thread() const;
    /** @brief 停止异步循环器并等待工作线程结束；析构不抛出异常。 */
    ~AsyncLooper() noexcept;

  private:
    // 接收、输出和刷新请求分别计数，所有字段由队列锁保护。
    struct FlushState {
        uint64_t accepted = 0;        // 已接收的记录数
        uint64_t completed = 0;       // 已处理的记录数
        uint64_t flush_target = 0;    // 当前刷新请求的记录完成目标
        uint64_t flush_requested = 0; // 已发起的刷新请求编号
        uint64_t flush_completed = 0; // 已完成的刷新请求编号
    };

    /** @brief 工作线程入口，交换缓冲区、逐条处理消费数据并记录完成状态。 */
    void thread_entry();
    void record_exception(std::exception_ptr exception);

    AsyncType looper_type_; // 固定容量或允许扩容的背压策略
    bool stop_ = false;     // 停止标志，由队列锁保护
    Buffer pro_buf_;        // 生产缓冲区
    Buffer con_buf_;        // 消费缓冲区
    std::mutex mutex_;        // 保护交换、接收与完成状态
    std::condition_variable cond_pro_; // 生产者条件变量
    std::condition_variable cond_con_; // 消费者条件变量
    std::condition_variable cond_done_; // 刷新完成条件变量
    std::mutex stop_mutex_; // 串行化 flush 与 stop，避免并发 join
    std::thread thread_;    // 工作线程
    std::thread::id worker_id_; // 构建后不再修改，用于检测工作线程重入
    Functor callback_;      // 单条记录的输出回调
    FlushFunctor flush_callback_; // sink 刷新回调
    std::chrono::milliseconds milliseco_; // 小批量日志的最大等待时间
    std::exception_ptr callback_exception_; // 保留首个后台异常
    FlushState flush_state_; // flush 只等待自己的完成条件
};
} // namespace detail
} // namespace zlog

#endif // ZLOG_INTERNAL_LOOPER_H_
