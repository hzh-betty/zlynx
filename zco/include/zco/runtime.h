#ifndef ZLYNX_ZCO_INCLUDE_ZCO_RUNTIME_H_
#define ZLYNX_ZCO_INCLUDE_ZCO_RUNTIME_H_

#include "zco/deadline.h"
#include "zco/result.h"
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>

namespace zco {
namespace detail {
struct Completion;
struct Submission;
} // namespace detail

using Task = std::function<void()>;

struct TaskId {
    uint64_t value = 0;

    explicit operator bool() const { return value != 0; }
};

inline bool operator==(TaskId a, TaskId b) { return a.value == b.value; }

inline bool operator!=(TaskId a, TaskId b) { return !(a == b); }
enum class StackModel { kShared, kIndependent };

struct RuntimeOptions {
    size_t worker_count =
        0; // 零表示使用硬件并发数；无法获取时至少使用一个线程。
    size_t stack_size = 256 * 1024;
    size_t shared_stack_count = 8;
    StackModel stack_model = StackModel::kShared;
};

/**
  * TaskStatus 状态转换图（任务对外状态）
  *
  *                         +-----------+
  *                         |  pending  |
  *                         +-----------+
  *                               |
  *                        建立执行记录成功
  *                               |
  *                               v
  *                         +-----------+
  *                         |  running  |
  *                         +-----------+
  *                               |
  *              +----------------+----------------+
  *              |                |                |
  *           正常返回         异常退出       TaskCanceled 退出
  *              |                |                |
  *              v                v                v
  *        +-----------+     +----------+     +----------+
  *        | succeeded |     |  failed  |     | canceled |
  *        +-----------+     +----------+     +----------+
  *
  * 状态说明：
  *   pending   : 已提交，尚未建立执行记录
  *   running   : 已建立执行记录，包含就绪、执行和等待阶段
  *   succeeded : 任务正常返回
  *   failed    : 建立执行记录失败，或执行过程中异常退出
  *   canceled  : 执行前被取消，或 TaskCanceled 异常逃出任务入口
  *
  * 补充转换：
  *   创建 Completion -> pending
  *   pending -> failed   : 建立执行记录失败
  *   pending -> canceled : 停止时任务尚未开始
  *
  * 注意：
  *   yield、park 不改变 TaskStatus，任务仍为 running。
  *   等待返回 canceled 不自动决定任务终态；正常返回仍可 succeeded。
  *   已开始任务先释放执行资源，再发布终态并唤醒 join 等待者。
  *   join 超时或丢弃 TaskHandle 不会取消任务。
*/
enum class TaskStatus { pending, running, succeeded, failed, canceled };

/**
 * @brief 共享任务完成状态的句柄，不拥有运行时。
 * @note 丢弃句柄不会取消任务；join() 观察结果，exception() 返回捕获的异常。
 *  Worker 中的任务 ──shared_ptr ──┐
                                  ├──→ Completion
    调用方的  TaskHandle  ─────────┘
 */
class TaskHandle {
  public:
    TaskHandle() = default;
    TaskId id() const;
    TaskStatus status() const;

    /**
     * @brief 等待任务完成，并重新抛出任务中捕获的异常。
     * @return 任务取消或等待超时时返回对应错误；等待超时不会取消目标任务。
     * @note 未观察的异常保留在完成状态中，不自动记录日志。
     * @throws std::logic_error 句柄为空，或任务试图等待自身。
     */
    Result<void> join(Deadline deadline = {}) const;
    std::exception_ptr exception() const;

    explicit operator bool() const { return bool(state_); }

    explicit TaskHandle(std::shared_ptr<detail::Completion> state);

  private:
    std::shared_ptr<detail::Completion> state_;
};

/**
 * @brief 固定向某个工作线程提交任务的执行器。
 * @note
 * 仅弱引用提交端点，不延长运行时生命周期；运行时停止或销毁后提交返回错误。
 */
class Executor {
  public:
    Executor() = default;

    /**
     * @brief 提交固定到该工作线程的任务，不能被其他线程窃取。
     * @return 空任务返回 invalid_argument；端点失效或停止接收时返回取消错误。
     * @note task 中捕获的引用必须存活至任务执行结束。
     */
    Result<TaskHandle> spawn(Task task) const;
    bool valid() const;

    size_t index() const { return index_; }

    bool same_runtime(const Executor &other) const;

  private:
    friend class Runtime;
    friend Executor current_executor();

    Executor(std::weak_ptr<detail::Submission> endpoint, size_t index)
        : endpoint_(std::move(endpoint)), index_(index) {}

    std::weak_ptr<detail::Submission> endpoint_;
    size_t index_ = 0;
};

/**
 * @brief 管理协程任务的工作线程和执行资源。
 * @note 析构会请求停止并等待工作线程；必须在本运行时的工作线程之外析构。
 * 停止依赖任务在让出执行或等待处协作，不能抢占持续运行的用户代码。
 */
class Runtime {
  public:
    /**
     * @brief 按配置创建并启动工作线程。
     * @throws std::invalid_argument 栈大小小于 16 KiB、超过 size_t
     * 最大值的一半， 栈模式不支持，或共享栈模式下 shared_stack_count 为零。
     */
    explicit Runtime(RuntimeOptions options = {});

    /**
     * @brief 请求停止并等待所有工作线程退出。
     * @pre 调用线程不属于本运行时的工作线程。
     * @warning 在自身工作线程析构会使 join() 抛出异常，进而终止程序。
     */
    ~Runtime();
    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;

    /**
     * @brief 提交可被其他工作线程窃取的任务。
     * @return 空任务返回 invalid_argument；停止接收后返回取消错误。
     * @note task 中捕获的引用必须存活至任务执行结束。
     */
    Result<TaskHandle> spawn(Task task);

    /**
     * @brief 获取固定到指定工作线程的执行器。
     * @throws std::out_of_range index 不小于 worker_count()。
     */
    Executor executor(size_t index) const;
    size_t worker_count() const;
    const RuntimeOptions &options() const;

    /**
     * @brief 停止接收新任务并通知工作线程停止，可在本运行时任务内调用。
     * @note 不等待线程退出；任务通过让出执行或等待点协作响应停止。
     */
    void request_stop();

    /**
     * @brief 等待所有工作线程退出，再释放它们持有的执行资源。
     * @pre 调用线程不属于本运行时的工作线程；通常先调用 request_stop()。
     * @throws std::logic_error 从本运行时的工作线程调用。
     * @note 等待依赖任务协作，不会强制释放仍在执行的协程栈。
     */
    void join();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace zco

#endif
