#include "hook.h"

#include "io_manager.h"

// 需要hook的函数列表
#define HOOK_FUN(XX) \
    XX(sleep) \
    XX(usleep) \
    XX(nanosleep) \
    XX(socket) \
    XX(connect) \
    XX(accept) \
    XX(read) \
    XX(readv) \
    XX(recv) \
    XX(recvfrom) \
    XX(recvmsg) \
    XX(write) \
    XX(writev) \
    XX(send) \
    XX(sendto) \
    XX(sendmsg) \
    XX(close) \
    XX(fcntl) \
    XX(ioctl) \
    XX(getsockopt) \
    XX(setsockopt)

namespace zlynx
{
    static thread_local bool t_hook_enable = false;
    bool is_hook_enable()
    {
        return t_hook_enable;
    }

    void set_hook_enable(const bool flag)
    {
        t_hook_enable = flag;
    }

    void hook_init()
    {
        static bool is_inited = false;
        if (is_inited) return;
        is_inited = true;

        // sleep_f = (sleep_fun)dlsym(RTLD_NEXT, "sleep"); -> dlsym -> fetch the original symbols/function
#define XX(name) name##_f = (name##_fun)dlsym(RTLD_NEXT, #name);
        HOOK_FUN(XX)
#undef XX
    }

    struct HookIniter
    {
        HookIniter()
        {
            hook_init();
        }
    };

    static HookIniter s_init_hook;
} // namespace zlynx

struct timer_info
{
    int cancelled = 0;
};
