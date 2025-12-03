#ifndef ZLYNX_SEMAPHORE_H_
#define ZLYNX_SEMAPHORE_H_
#include <cstdint>
#include <semaphore.h>

#include "noncopyable.h"

namespace zlynx
{
    /**
     * @brief 信号量类
     * 提供基本的信号量操作接口
     */
    class Semaphore: public NonCopyable
    {
    public:
        /**
         * @brief 构造函数
         * @param count 初始信号量计数值
         */
        explicit Semaphore(uint32_t count = 0);

        ~Semaphore();

        /**
         * @brief 等待信号量
         * 如果信号量计数值大于0，则将其减1并继续执行；
         * 否则阻塞当前线程直到信号量可用。
         */
        void wait();

        /**
         * @brief 释放信号量
         * 将信号量计数值加1，唤醒一个等待的线程（如果有的话）。
         */
        void notify();

    private:
        sem_t sem_{}; ///< 底层信号量对象
    };
} // namespace zlynx

#endif //ZLYNX_SEMAPHORE_H_
