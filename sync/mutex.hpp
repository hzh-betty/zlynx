#ifndef ZLYNX_MUTEX_H_
#define ZLYNX_MUTEX_H_
#include <pthread.h>

#include <atomic>

#include "noncopyable.h"

namespace zlynx
{
    /**
     * @brief 互斥锁锁定类模板
     * 提供RAII风格的互斥锁管理
     * @tparam T 互斥锁类型
     */
    template <typename T>
    class LockGuardImpl
    {
    public:
        explicit LockGuardImpl(T& lock)
            : mutex_(lock), locked_(false)
        {
            lock();
        }

        ~LockGuardImpl()
        {
            unlock();
        }

        void lock()
        {
            if (!locked_)
            {
                mutex_.lock();
                locked_ = true;
            }
        }
        void unlock()
        {
            if (locked_)
            {
                mutex_.unlock();
                locked_ = false;
            }
        }
    private:
        T& mutex_; // 互斥锁对象
        std::atomic<bool> locked_; // 锁定状态
    };

    /**
     * @brief 互斥锁类
     * 封装了pthread互斥锁的基本操作
     */
    class Mutex: public NonCopyable
    {
    public:
        using Lock = LockGuardImpl<Mutex>;

        Mutex()
        {
            pthread_mutex_init(&mutex_, nullptr);
        }
        void lock()
        {
            pthread_mutex_lock(&mutex_);
        }
        void unlock()
        {
            pthread_mutex_unlock(&mutex_);
        }
        ~Mutex()
        {
            pthread_mutex_destroy(&mutex_);
        }
    private:
        pthread_mutex_t mutex_; // 底层互斥锁对象
    };
} // namespace zlynx

#endif //ZLYNX_MUTEX_H_
