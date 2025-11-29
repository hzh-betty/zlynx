#ifndef ZLYNX_FIBER_H_
#define ZLYNX_FIBER_H_
#include <ucontext.h>

#include <memory>
#include <atomic>
#include <functional>

namespace zlynx
{
    class Fiber: public std::enable_shared_from_this<Fiber>
    {
    public:
        using ptr = std::shared_ptr<Fiber>;

        enum class State {kReady, kRunning, kTerminated};

        explicit Fiber(std::function<void()> func,size_t stack_size = 128 * 1024);
        ~Fiber();

        void resume(ptr caller = nullptr); // 恢复协程执行
        void yield(); // 挂起协程

        void reset(std::function<void()> func); // 重置协程

        uint64_t id() const noexcept { return id_; }
        State state() const noexcept { return state_; }

        static void set_fiber(ptr fiber) noexcept;
        static ptr get_fiber() noexcept;
        static void main_func() noexcept;
    private:
        Fiber(); // 主协程用

        State state_ = State::kReady; // 协程状态

        uint64_t id_ = 0; // 协程ID
        size_t stack_size_ = 0; // 协程栈大小

        ucontext_t ctx_{}; // 协程上下文
        void* stack_ptr_ = nullptr; // 协程栈指针

        Fiber::ptr caller_fiber_ = nullptr; // 调用者协程指针

        std::function<void()> callback_; // 协程执行的回调函数
        std::exception_ptr exception_; // 协程异常指针
        static std::atomic<int> total_fiber_count; // 全局协程计数器
    };
}// namespace zlynx

#endif //ZLYNX_FIBER_H_
