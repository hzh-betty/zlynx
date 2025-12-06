#ifndef ZLYNX_RW_MUTEX_H_
#define ZLYNX_RW_MUTEX_H_
#include <pthread.h>

#include <atomic>

#include"noncopyable.h"
namespace zlynx
{
    template<class T>
    class ReadLockGuard
    {
    public:
        explicit ReadLockGuard(T& rwlock)
            : rwmutex_(rwlock), locked_(false)
        {
            lock();
        }

        ~ReadLockGuard()
        {
            unlock();
        }

        void lock()
        {
            if (!locked_)
            {
                rwmutex_.rdlock();
                locked_ = true;
            }
        }
        void unlock()
        {
            if (locked_)
            {
                rwmutex_.unlock();
                locked_ = false;
            }
        }
    private:
        T& rwmutex_; // 读锁对象
        std::atomic<bool> locked_; // 锁定状态
    };

    template<class T>
    class WriteLockGuard
    {
    public:
        explicit WriteLockGuard(T& rwlock)
            : rwmutex_(rwlock), locked_(false)
        {
            lock();
        }

        ~WriteLockGuard()
        {
            unlock();
        }

        void lock()
        {
            if (!locked_)
            {
                rwmutex_.wrlock();
                locked_ = true;
            }
        }
        void unlock()
        {
            if (locked_)
            {
                rwmutex_.unlock();
                locked_ = false;
            }
        }
    private:
        T& rwmutex_; // 写锁对象
        std::atomic<bool> locked_; // 锁定状态
    };

    class RWMutex: public NonCopyable
    {
    public:
        using ReadLock = ReadLockGuard<RWMutex>;
        using WriteLock = WriteLockGuard<RWMutex>;
        RWMutex()
        {
            pthread_rwlock_init(&rwlock_, nullptr);
        }
        ~RWMutex()
        {
            pthread_rwlock_destroy(&rwlock_);
        }

        void rdlock()
        {
            pthread_rwlock_rdlock(&rwlock_);
        }
        void wrlock()
        {
            pthread_rwlock_wrlock(&rwlock_);
        }
        void unlock()
        {
            pthread_rwlock_unlock(&rwlock_);
        }
    private:
        pthread_rwlock_t rwlock_{}; // 读写锁对象
    };
}// namespace zlynx
#endif //ZLYNX_RW_MUTEX_H_
