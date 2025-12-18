#include "runtime/fiber.h"
#include "runtime/shared_stack_pool.h"
#include "util/thread_context.h"
#include "zcoroutine_logger.h"
#include <cassert>
#include <cstring>

namespace zcoroutine {

// 静态成员初始化
std::atomic<uint64_t> Fiber::s_fiber_count_{0};

// 主协程构造函数
Fiber::Fiber()
    : name_("main_fiber")
    , id_(0)
    , state_(State::kRunning)
    , stack_size_(0)
    , context_(std::make_unique<Context>()) {
    
    // 主协程直接获取当前上下文
    context_->get_context();
    
    // 设置为当前协程
    ThreadContext::SetCurrentFiber(this);
    
    ZCOROUTINE_LOG_INFO("Main fiber created: name={}, id={}", name_, id_);
}

// 普通协程构造函数
Fiber::Fiber(std::function<void()> func,
             size_t stack_size,
             const std::string& name,
             bool use_shared_stack,
             SharedStackPool* stack_pool)
    : callback_(std::move(func))
    , stack_size_(stack_size)
    , use_shared_stack_(use_shared_stack)
    , stack_pool_(stack_pool)
    , context_(std::make_unique<Context>()) {
    
    // 分配全局唯一ID
    id_ = s_fiber_count_.fetch_add(1, std::memory_order_relaxed);
    
    // 设置协程名称
    if (name.empty()) {
        name_ = "fiber_" + std::to_string(id_);
    } else {
        name_ = name + "_" + std::to_string(id_);
    }
    
    ZCOROUTINE_LOG_DEBUG("Fiber creating: name={}, id={}, stack_size={}, use_shared_stack={}", 
                         name_, id_, stack_size_, use_shared_stack_);
    
    // 分配栈内存
    if (use_shared_stack_) {
        // 共享栈模式
        if (!stack_pool_) {
            ZCOROUTINE_LOG_FATAL("Fiber creation failed: shared stack mode requires stack_pool, name={}, id={}", 
                                 name_, id_);
            abort();
        }
        shared_stack_ = stack_pool_->allocate_stack();
        stack_ptr_ = shared_stack_->stack_buffer;
        ZCOROUTINE_LOG_DEBUG("Fiber using shared stack: name={}, id={}, buffer={}", 
                             name_, id_, static_cast<void*>(stack_ptr_));
    } else {
        // 独立栈模式
        stack_ptr_ = StackAllocator::allocate(stack_size_);
        if (!stack_ptr_) {
            ZCOROUTINE_LOG_FATAL("Fiber stack allocation failed: name={}, id={}, size={}", 
                                 name_, id_, stack_size_);
            abort();
        }
        ZCOROUTINE_LOG_DEBUG("Fiber using independent stack: name={}, id={}, ptr={}, size={}", 
                             name_, id_, static_cast<void*>(stack_ptr_), stack_size_);
    }
    
    // 创建上下文
    context_->make_context(stack_ptr_, stack_size_, Fiber::main_func);
    
    ZCOROUTINE_LOG_INFO("Fiber created: name={}, id={}, shared_stack={}", 
                        name_, id_, use_shared_stack_);
}

Fiber::~Fiber() {
    ZCOROUTINE_LOG_DEBUG("Fiber destroying: name={}, id={}, state={}", 
                         name_, id_, static_cast<int>(state_));
    
    // 释放栈内存
    if (stack_ptr_ && !use_shared_stack_) {
        StackAllocator::deallocate(stack_ptr_, stack_size_);
        stack_ptr_ = nullptr;
        ZCOROUTINE_LOG_DEBUG("Fiber stack deallocated: name={}, id={}", name_, id_);
    }
    
    // 共享栈模式释放保存缓冲区
    if (save_buffer_) {
        save_buffer_.reset();
        ZCOROUTINE_LOG_DEBUG("Fiber save buffer released: name={}, id={}", name_, id_);
    }
}

void Fiber::resume(ptr caller) {
    assert(state_ != State::kTerminated && "Cannot resume terminated fiber");
    assert(state_ != State::kRunning && "Fiber is already running");
    
    // 保存调用者
    if (caller) {
        caller_fiber_ = caller;
    }
    
    // 设置为当前协程
    Fiber* prev_fiber = ThreadContext::GetCurrentFiber();
    ThreadContext::SetCurrentFiber(this);
    
    // 更新状态
    State prev_state = state_;
    state_ = State::kRunning;
    
    ZCOROUTINE_LOG_DEBUG("Fiber resume: name={}, id={}, prev_state={}, caller={}", 
                         name_, id_, static_cast<int>(prev_state), 
                         caller ? caller->name() : "none");
    
    // 共享栈模式需要恢复栈数据
    if (use_shared_stack_ && save_buffer_) {
        memcpy(stack_sp_, save_buffer_.get(), save_size_);
        ZCOROUTINE_LOG_DEBUG("Fiber stack restored: name={}, id={}, sp={}, size={}", 
                             name_, id_, static_cast<void*>(stack_sp_), save_size_);
    }
    
    // 切换上下文
    if (prev_fiber && prev_fiber->context_) {
        Context::swap_context(prev_fiber->context_.get(), context_.get());
    } else {
        // 没有前一个协程，创建临时主协程上下文
        Context temp_ctx;
        temp_ctx.get_context();
        Context::swap_context(&temp_ctx, context_.get());
    }
    
    // 协程执行完毕后会切换回来，恢复前一个协程
    ThreadContext::SetCurrentFiber(prev_fiber);
}

void Fiber::yield() {
    Fiber* cur_fiber = ThreadContext::GetCurrentFiber();
    if (!cur_fiber) {
        ZCOROUTINE_LOG_WARN("Fiber::yield failed: no current fiber to yield");
        return;
    }
    
    assert(cur_fiber->state_ == State::kRunning && "Can only yield running fiber");
    
    // 更新状态
    cur_fiber->state_ = State::kSuspended;
    
    ZCOROUTINE_LOG_DEBUG("Fiber yield: name={}, id={}", cur_fiber->name_, cur_fiber->id_);
    
    // 共享栈模式需要保存栈数据
    if (cur_fiber->use_shared_stack_ && cur_fiber->shared_stack_) {
        // 计算栈使用量
        char dummy;
        char* cur_sp = &dummy;
        cur_fiber->stack_sp_ = cur_sp;
        cur_fiber->save_size_ = cur_fiber->shared_stack_->stack_bp - cur_sp;
        
        // 分配保存缓冲区
        if (!cur_fiber->save_buffer_ || cur_fiber->save_size_ > cur_fiber->stack_size_) {
            cur_fiber->save_buffer_ = std::make_unique<char[]>(cur_fiber->save_size_);
        }
        
        // 拷贝栈数据
        memcpy(cur_fiber->save_buffer_.get(), cur_sp, cur_fiber->save_size_);
        
        ZCOROUTINE_LOG_DEBUG("Fiber stack saved: name={}, id={}, sp={}, size={}", 
                             cur_fiber->name_, cur_fiber->id_, 
                             static_cast<void*>(cur_sp), cur_fiber->save_size_);
    }
    
    // 切换回调用者或主协程
    auto caller = cur_fiber->caller_fiber_.lock();
    if (caller && caller->context_) {
        ThreadContext::SetCurrentFiber(caller.get());
        Context::swap_context(cur_fiber->context_.get(), caller->context_.get());
    } else {
        // 没有调用者，创建临时上下文切换
        Context temp_ctx;
        temp_ctx.get_context();
        ThreadContext::SetCurrentFiber(nullptr);
        Context::swap_context(cur_fiber->context_.get(), &temp_ctx);
    }
}

void Fiber::reset(std::function<void()> func) {
    assert(state_ == State::kTerminated && "Can only reset terminated fiber");
    
    callback_ = std::move(func);
    state_ = State::kReady;
    exception_ = nullptr;
    
    // 重新创建上下文
    context_->make_context(stack_ptr_, stack_size_, Fiber::main_func);
    
    ZCOROUTINE_LOG_DEBUG("Fiber reset: name={}, id={}", name_, id_);
}

void Fiber::main_func() {
    Fiber* cur_fiber = ThreadContext::GetCurrentFiber();
    assert(cur_fiber && "No current fiber in main_func");
    
    ZCOROUTINE_LOG_DEBUG("Fiber main_func starting: name={}, id={}", 
                         cur_fiber->name_, cur_fiber->id_);
    
    try {
        // 执行协程函数
        cur_fiber->callback_();
        cur_fiber->callback_ = nullptr;
        cur_fiber->state_ = State::kTerminated;
        
        ZCOROUTINE_LOG_INFO("Fiber terminated normally: name={}, id={}", 
                            cur_fiber->name_, cur_fiber->id_);
    } catch (const std::exception& e) {
        // 捕获标准异常
        cur_fiber->exception_ = std::current_exception();
        cur_fiber->state_ = State::kTerminated;
        
        ZCOROUTINE_LOG_ERROR("Fiber terminated with exception: name={}, id={}, what={}", 
                             cur_fiber->name_, cur_fiber->id_, e.what());
    } catch (...) {
        // 捕获其他异常
        cur_fiber->exception_ = std::current_exception();
        cur_fiber->state_ = State::kTerminated;
        
        ZCOROUTINE_LOG_ERROR("Fiber terminated with unknown exception: name={}, id={}", 
                             cur_fiber->name_, cur_fiber->id_);
    }
    
    // 协程结束，切换回调用者
    auto caller = cur_fiber->caller_fiber_.lock();
    if (caller && caller->context_) {
        ZCOROUTINE_LOG_DEBUG("Fiber switching back to caller: name={}, id={}, caller={}", 
                             cur_fiber->name_, cur_fiber->id_, caller->name());
        ThreadContext::SetCurrentFiber(caller.get());
        Context::swap_context(cur_fiber->context_.get(), caller->context_.get());
    }
}

Fiber* Fiber::get_this() {
    return ThreadContext::GetCurrentFiber();
}

void Fiber::set_this(Fiber* fiber) {
    ThreadContext::SetCurrentFiber(fiber);
}

} // namespace zcoroutine
