#ifndef ZLYNX_SPINLOCK_H_
#define ZLYNX_SPINLOCK_H_
#include "mutex.hpp"

namespace zlynx
{
    class SpinLock:public NonCopyable
    {
    public:
        using Lock = LockGuardImpl<SpinLock>;

        SpinLock()
        {
            pthread_spin_init(&spinlock_, PTHREAD_PROCESS_PRIVATE);
        }
        ~SpinLock()
        {
            pthread_spin_destroy(&spinlock_);
        }

        void lock()
        {
            pthread_spin_lock(&spinlock_);
        }
        void unlock()
        {
            pthread_spin_unlock(&spinlock_);
        }
    private:
        pthread_spinlock_t spinlock_;
    };
} // namespace zlynx
#endif //ZLYNX_SPINLOCK_H_
