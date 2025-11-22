#include "thread.h"

namespace zlynx
{
    static thread_local Thread* t_thread = nullptr;
    static thread_local std::string t_thread_name = "UNKNOWN";

    Thread::Thread(std::function<void()> func, std::string name)
        : name_(std::move(name)),
          callback_(std::move(func)),
          sem_(0)
    {
        pthread_create(&thread_, nullptr, [](void* arg) -> void*
        {
            auto* thread = static_cast<Thread*>(arg);
            t_thread = thread;
            t_thread_name = thread->name_;
            thread->pid_ = static_cast<pid_t>(pthread_self());
            thread->sem_.notify();
            thread->callback_();
            return nullptr;
        }, this);
        sem_.wait();
    }

    Thread::~Thread()
    {
        if (thread_)
        {
            pthread_detach(thread_);
        }
    }

    void Thread::join()
    {
        if (thread_)
        {
            pthread_join(thread_, nullptr);
            thread_ = 0;
        }
    }

    Thread* Thread::get_this()
    {
        return t_thread;
    }

    const std::string& Thread::get_name()
    {
        return t_thread_name;
    }

    void Thread::set_name(std::string name)
    {
        if (t_thread)
        {
            t_thread->name_ = std::move(name);
        }
        t_thread_name = name;
    }
} // namespace zlynx