#include "sem.h"

#include <stdexcept>
namespace zlynx
{
    Semaphore::Semaphore(uint32_t count)
    {
        if (sem_init(&sem_, 0, static_cast<unsigned int>(count)) != 0)
        {
            throw std::runtime_error("sem_init failed");
        }
    }

    Semaphore::~Semaphore()
    {
        sem_destroy(&sem_);
    }
    void Semaphore::wait()
    {
        while (sem_wait(&sem_) != 0)
        {
            if (errno != EINTR)
            {
                throw std::runtime_error("sem_wait failed");
            }
        }
    }

    void Semaphore::notify()
    {
        if (sem_post(&sem_) != 0)
        {
            throw std::runtime_error("sem_post failed");
        }
    }
}// namespace zlynx