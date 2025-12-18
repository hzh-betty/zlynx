#ifndef ZCOROUTINE_FIBER_H_
#define ZCOROUTINE_FIBER_H_

#include <memory>
#include <functional>
#include <string>
#include <atomic>
#include "runtime/context.h"
#include "runtime/stack_allocator.h"

namespace zcoroutine {

// 前向声明
class SharedStackPool;
struct StackMem;

/**
 * @brief 协程类
 * 管理协程的生命周期、状态和上下文切换
 * 支持独立栈和共享栈两种模式
 */
class Fiber : public std::enable_shared_from_this<Fiber> {
public:
    using ptr = std::shared_ptr<Fiber>;

    /**
     * @brief 协程状态枚举
     */
    enum class State {
        kReady,         // 就绪态，等待调度
        kRunning,       // 运行态，正在执行
        kSuspended,     // 挂起态，主动让出CPU
        kTerminated     // 终止态，执行完毕
    };

    /**
     * @brief 构造函数
     * @param func 协程执行函数
     * @param stack_size 栈大小，默认128KB
     * @param name 协程名称，默认为空（自动生成fiber_id）
     * @param use_shared_stack 是否使用共享栈，默认false
     * @param stack_pool 共享栈池指针，use_shared_stack为true时必须提供
     */
    explicit Fiber(std::function<void()> func,
                   size_t stack_size = StackAllocator::kDefaultStackSize,
                   const std::string& name = "",
                   bool use_shared_stack = false,
                   SharedStackPool* stack_pool = nullptr);

    /**
     * @brief 析构函数
     */
    ~Fiber();

    /**
     * @brief 恢复协程执行
     * @param caller 调用者协程，默认为nullptr
     */
    void resume(ptr caller = nullptr);

    /**
     * @brief 挂起协程
     * 将控制权交还给调用者或主协程
     */
    static void yield();

    /**
     * @brief 重置协程
     * @param func 新的执行函数
     * 用于协程池复用协程对象
     */
    void reset(std::function<void()> func);

    /**
     * @brief 获取协程名称
     * @return 协程名称（格式：name_id或fiber_id）
     */
    std::string name() const { return name_; }

    /**
     * @brief 获取协程ID
     * @return 协程全局唯一ID
     */
    uint64_t id() const { return id_; }

    /**
     * @brief 获取协程状态
     * @return 当前状态
     */
    State state() const { return state_; }

    /**
     * @brief 是否使用共享栈
     * @return true表示共享栈，false表示独立栈
     */
    bool is_shared_stack() const { return use_shared_stack_; }

    /**
     * @brief 协程主函数（静态）
     * 在协程上下文中执行
     */
    static void main_func();

    /**
     * @brief 获取当前执行的协程
     * @return 当前协程指针
     */
    static Fiber* get_this();

    /**
     * @brief 设置当前协程
     * @param fiber 协程指针
     */
    static void set_this(Fiber* fiber);

private:
    /**
     * @brief 主协程构造函数（私有）
     * 用于创建线程的主协程
     */
    Fiber();

    std::string name_;                      // 协程名称
    uint64_t id_ = 0;                       // 协程唯一ID
    State state_ = State::kReady;           // 协程状态
    size_t stack_size_ = 0;                 // 栈大小

    std::unique_ptr<Context> context_;      // 上下文对象
    void* stack_ptr_ = nullptr;             // 栈指针（独立栈模式）

    std::weak_ptr<Fiber> caller_fiber_;     // 调用者协程（弱引用防止循环）

    std::function<void()> callback_;        // 协程执行函数
    std::exception_ptr exception_;          // 协程异常指针

    // 共享栈相关
    bool use_shared_stack_ = false;         // 是否使用共享栈
    SharedStackPool* stack_pool_ = nullptr; // 共享栈池指针
    StackMem* shared_stack_ = nullptr;      // 当前使用的共享栈
    std::unique_ptr<char[]> save_buffer_;   // 共享栈保存缓冲区
    size_t save_size_ = 0;                  // 保存数据大小
    char* stack_sp_ = nullptr;              // 栈指针位置

    // 全局协程计数器（线程安全）
    static std::atomic<uint64_t> s_fiber_count_;
};

} // namespace zcoroutine

#endif // ZCOROUTINE_FIBER_H_
