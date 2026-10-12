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
static constexpr size_t kFlushBufferSize =
    kDefaultBufferSize / 32; // 刷新缓冲区大小阈值

// 长度头计入容量；零长度日志也占用空间并保留记录边界。
static constexpr size_t kRecordHeaderSize = sizeof(size_t);

/**
 * @brief 将并发提交的完整记录交给单个后台线程输出，并提供刷新屏障。
 *
 * 队列锁保护生产缓冲区和完成状态；交换后消费缓冲区仅由后台线程访问。
 * 输出和刷新回调均在队列锁外执行，不得重入本实例的 push、flush 或 stop。
 * 本对象须在后台线程之外销毁，且销毁前须结束外部访问。
 */
class AsyncLooper {
  public:
    using Functor =
        std::function<void(const char *, size_t)>; // 单条日志回调函数类型
    using FlushFunctor = std::function<void()>;

    /**
     * @brief 启动后台消费者，并保存输出与刷新回调。
     *
     * @param func 逐条输出回调，字节指针仅在本次回调期间有效。
     * @param looper_type 固定容量等待或允许扩容的背压策略，两者均使用队列锁。
     * @param milliseco
     * 消费者小批量等待的轮询间隔，必须大于零；不限制生产者等待时长。
     * @param flush 可选的输出端刷新回调。
     * @throws std::invalid_argument milliseco 不大于零。
     */
    AsyncLooper(Functor func, AsyncType looper_type,
                std::chrono::milliseconds milliseco, FlushFunctor flush = {});

    // C++11 的普通 new 不保证过度对齐，为内部缓存行对齐提供匹配的分配与释放。
    static void *operator new(size_t size);
    static void operator delete(void *memory) noexcept;

    /**
     * @brief 复制一条完整记录到生产缓冲区；达到模式容量时等待消费者释放空间。
     *
     * @param data 输入字节，len 为 0 时允许为空；返回后无需保留输入存储。
     * @param len 记录字节数，长度头也计入容量预算。
     * @throws std::length_error 单条记录与长度头超过模式允许的容量。
     * @throws std::runtime_error 循环器已停止，包括等待期间停止。
     * @throws std::logic_error 在本实例后台线程中重入。
     * @throws std::invalid_argument 非零长度时 data 为空。
     */
    void push(const char *data, size_t len);

    /**
     * @brief 等待本次请求锁定的已接收记录完成回调，并完成一次输出端刷新。
     *
     * 后续提交的记录不扩展本次等待目标；与 stop 串行执行。
     * 后台回调失败后仍推进完成计数，保存的首个异常在此处重新抛出。
     * @throws std::logic_error 在本实例后台线程中重入。
     */
    void flush();

    /**
     * @brief 拒绝后续提交，排空已接收记录、刷新输出端并等待后台线程结束。
     *
     * 可重复调用；结束后重新抛出保存的首个后台回调异常。
     * @throws std::logic_error 在本实例后台线程中重入。
     */
    void stop();
    bool is_worker_thread() const;

    /**
     * @brief 停止异步循环器并等待工作线程结束；析构不抛出异常。
     */
    ~AsyncLooper() noexcept;

  private:
    // 所有计数由队列锁保护；completed 统计已尝试执行回调的记录，含失败记录。
    // 刷新请求编号与记录目标分开，保证空队列的 flush 也执行刷新回调。
    struct FlushState {
        uint64_t accepted = 0;        // 已接收的记录数
        uint64_t completed = 0;       // 已处理的记录数
        uint64_t flush_target = 0;    // 当前刷新请求的记录完成目标
        uint64_t flush_requested = 0; // 已发起的刷新请求编号
        uint64_t flush_completed = 0; // 已完成的刷新请求编号
    };

    /**
     * @brief 工作线程入口，交换缓冲区、逐条处理消费数据并记录完成状态。
     */
    void thread_entry();
    void record_exception(std::exception_ptr exception);

    AsyncType looper_type_;             // 固定容量或允许扩容的背压策略
    bool stop_ = false;                 // 停止标志，由队列锁保护
    Buffer pro_buf_;                    // 生产缓冲区
    Buffer con_buf_;                    // 消费缓冲区
    std::mutex mutex_;                  // 保护交换、接收与完成状态
    std::condition_variable cond_pro_;  // 生产者条件变量
    std::condition_variable cond_con_;  // 消费者条件变量
    std::condition_variable cond_done_; // 刷新完成条件变量
    std::mutex stop_mutex_;             // 串行化 flush 与 stop，避免并发 join
    std::thread thread_;                // 工作线程
    std::thread::id worker_id_;         // 构建后不再修改，用于检测工作线程重入
    Functor callback_;                  // 单条记录的输出回调
    FlushFunctor flush_callback_;       // sink 刷新回调
    std::chrono::milliseconds
        milliseco_; // 消费者的小批量轮询间隔，不是提交阻塞上限
    std::exception_ptr callback_exception_; // 保留首个后台异常
    FlushState flush_state_;                // flush 只等待自己的完成条件
};
} // namespace detail
} // namespace zlog

#endif
