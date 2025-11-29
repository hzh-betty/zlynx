#include "fiber.h"

#include <sys/mman.h>

#include <stdexcept>
#include <utility>

#include "zlynx_logger.h"

namespace zlynx
{
    static constexpr size_t PAGE_SIZE = 8 * 1024; // 8KB页面大小
    thread_local Fiber::ptr t_fiber = nullptr; // 当前运行的协程
    thread_local Fiber::ptr t_main_fiber = nullptr; // 主协程
    std::atomic<int> Fiber::total_fiber_count{0}; // 全局协程计数器初始化
    Fiber::Fiber(std::function<void()> func, size_t stack_size)
        :stack_size_(stack_size),
         callback_(std::move(func))
    {
        ++total_fiber_count;
        id_ = total_fiber_count.load();

        // mmap栈 + 底部guard_page
        stack_ptr_ = mmap(nullptr, stack_size_ + PAGE_SIZE,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
        if (stack_ptr_ == MAP_FAILED)
        {
            throw std::runtime_error("mmap failed");
        }

        // 设置guard_page不可访问, 目的是让栈溢出，直接导致段错误
        mprotect(stack_ptr_, PAGE_SIZE, PROT_NONE);

        if (getcontext(&ctx_) == -1)
        {
            throw std::runtime_error("getcontext failed");
        }

        // 设置栈指针和大小
        ctx_.uc_stack.ss_sp = static_cast<char*>(stack_ptr_) + PAGE_SIZE; // 栈顶
        ctx_.uc_stack.ss_size = stack_size_;
        ctx_.uc_link = nullptr;
        makecontext(&ctx_, &Fiber::main_func, 0);
    }

    Fiber::~Fiber()
    {
        --total_fiber_count;
        if (stack_ptr_)
            munmap(stack_ptr_, stack_size_ + PAGE_SIZE);
    }

    void Fiber::resume(ptr caller)
    {
        if (state_ == State::kTerminated) return;
        ZLYNX_LOG_DEBUG("Fiber::resume id={}", id_);

        if (caller)
        {
            caller_fiber_ = std::move(caller);
        }
        else
        {
            if (!t_main_fiber)
            {
               throw std::runtime_error("Fiber::resume called without main_fiber");
            }
            caller_fiber_ = t_main_fiber;
        }

        set_fiber(shared_from_this());
        state_ = State::kRunning;

        ZLYNX_LOG_DEBUG("Fiber::resume swapcontext from id={} to id={}", caller_fiber_->id_, id_);
        if (swapcontext(&caller_fiber_->ctx_, &ctx_) == -1) {
            throw std::runtime_error("swapcontext failed");
        }

        if (exception_) {
            std::rethrow_exception(exception_);
        }
    }

    void Fiber::yield()
    {
        ZLYNX_LOG_DEBUG("Fiber::yield id={}", id_);
        state_ = State::kReady;
        const auto caller = caller_fiber_ ? caller_fiber_ : t_main_fiber;

        if (!caller)
        {
            throw std::runtime_error("Fiber::yield called without main_fiber");
        }
        set_fiber(caller);

        ZLYNX_LOG_DEBUG("Fiber::yield swapcontext from id={} to id={}", id_, caller->id_);
        if (swapcontext(&ctx_, &caller->ctx_) == -1) {
            throw std::runtime_error("swapcontext failed");
        }
    }

    void Fiber::reset(std::function<void()> func)
    {
        if (getcontext(&ctx_) == -1)
        {
            throw std::runtime_error("getcontext failed");
        }

        callback_ = std::move(func);
        ctx_.uc_stack.ss_sp = static_cast<char*>(stack_ptr_) + PAGE_SIZE; // 栈顶
        ctx_.uc_stack.ss_size = stack_size_;
        ctx_.uc_link = nullptr;

        makecontext(&ctx_, &Fiber::main_func, 0);
        state_ = State::kReady;
    }

    void Fiber::set_fiber(ptr fiber) noexcept
    {
        t_fiber = std::move(fiber);
    }

    Fiber::ptr Fiber::get_fiber() noexcept
    {
        if (!t_fiber)
        {
            t_main_fiber = std::move(ptr(new Fiber()));
            t_fiber = t_main_fiber;
        }
        return t_fiber->shared_from_this(); // 返回执行自己的智能指针
    }

    void Fiber::main_func() noexcept
    {
        ZLYNX_LOG_DEBUG("Fiber::main_func start id={}", get_fiber()->id_);
        const std::shared_ptr<Fiber> guard = get_fiber();  // 保持存活


        try {
            guard->callback_();
        } catch (...) {
            guard->exception_ = std::current_exception(); // 跨协程传递异常
        }

        ZLYNX_LOG_DEBUG("Fiber::main_func end id={}", guard->id_);
        guard->callback_ = nullptr;
        guard->state_ = State::kTerminated;
        // guard->yield(); // FiberStack 切回 MainStack 能够执行后续逻辑

        // 直接切换上下文，不调用 yield()
        const auto caller = guard->caller_fiber_ ? guard->caller_fiber_ : t_main_fiber;
        set_fiber(caller);

        // 手动切换,避免在析构中操作
        if (swapcontext(&guard->ctx_, &caller->ctx_) == -1) {
            ZLYNX_LOG_ERROR("swapcontext failed in main_func");
        }
    }

    Fiber::Fiber()
    {
        ++total_fiber_count;
        state_ = State::kRunning;
        if (getcontext(&ctx_) == -1) {
            throw std::runtime_error("getcontext failed");
        }
    }

}// namespace zlynx