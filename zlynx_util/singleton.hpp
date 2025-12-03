#ifndef ZLYNX_SINGLETON_H
#define ZLYNX_SINGLETON_H

#include <utility>
#include "noncopyable.h"

namespace zlynx
{
    class Singleton : public NonCopyable
    {
    public:

        template<class T,class ... Args>
        static T &get_instance(Args&&... args)
        {
            static T instance(std::forward<Args>(args)...);
            return instance;
        }
    private:
        Singleton() = default;
        ~Singleton() = default;
    };
} //
#endif //ZLYNX_SINGLETON_H
