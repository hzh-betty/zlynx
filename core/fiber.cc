#include "fiber.h"

#include <sys/mman.h>

#include <stdexcept>

#include "zlynx_logger.h"

namespace zlynx
{
    static constexpr size_t PAGE_SIZE = 8 * 1024; // 8KB页面大小
    thread_local Fiber* t_fiber = nullptr; // 当前运行的协程
    thread_local std::unique_ptr<Fiber> t_main_fiber = nullptr; // 主协程
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
        mprotect(stack_ptr_, PAGE_SIZE, PROT_NONE); // 设置guard_page不可访问

        if (getcontext(&ctx_) == -1)
        {
            throw std::runtime_error("getcontext failed");
        }

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

    void Fiber::resume()
    {
        if (state_ == State::kTerminated) return;

        set_fiber(this);
        state_ = State::kRunning;

        if (swapcontext(&t_main_fiber->ctx_, &ctx_) == -1) {
            throw std::runtime_error("swapcontext failed");
        }

        if (exception_) {
            std::rethrow_exception(exception_);
        }
    }

    void Fiber::yield()
    {
        state_ = State::kReady;
        set_fiber(t_main_fiber.get());
        if (swapcontext(&ctx_, &t_main_fiber->ctx_) == -1) {
            throw std::runtime_error("swapcontext failed");
        }
    }

    void Fiber::set_fiber(Fiber *fiber) noexcept
    {
        t_fiber = fiber;
    }

    Fiber * Fiber::get_fiber() noexcept
    {
        if (!t_fiber)
        {
            t_main_fiber.reset(new Fiber());
            t_fiber = t_main_fiber.get();
        }
        return t_fiber;
    }

    void Fiber::main_func() noexcept
    {
        Fiber* f = get_fiber();
        std::shared_ptr<Fiber> guard = f->shared_from_this();  // 保持存活


        try {
            f->callback_();
        } catch (...) {
            f->state_ = State::kTerminated;
            f->exception_ = std::current_exception(); // 跨协程传递异常
        }

        f->callback_ = nullptr;
        f->state_ = State::kTerminated;
        f->yield(); // FiberStack 切回 MainStack 能够执行后续逻辑
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