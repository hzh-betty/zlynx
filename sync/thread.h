#ifndef ZLYNX_THREAD_H_
#define ZLYNX_THREAD_H_
#include <pthread.h>

#include <string>
#include <functional>
#include <memory>

#include "noncopyable.h"
#include "sem.h"
namespace zlynx
{
    class Thread:public NonCopyable
    {
    public:
        using ptr = std::unique_ptr<Thread>;

        Thread(std::function<void()> func,std::string name);

        ~Thread();

        pid_t pid() const { return pid_; }
        const std::string& name() const { return name_; }

        void join();

        static Thread* get_this();
        static const std::string& get_name();
        static void set_name(std::string name);

    private:
        pid_t pid_ = -1; // 线程ID
        pthread_t thread_ = 0; // 线程句柄
        std::string name_; // 线程名称
        std::function<void()> callback_; // 线程执行的回调函数
        Semaphore sem_; // 信号量，用于线程同步
    };

}// namespace zlynx



#endif //ZLYNX_THREAD_H_
