#include "zlynx_logger.h"

#include "fiber.h"

void test1()
{
    try
    {
        zlynx::Init();
        ZLYNX_LOG_INFO("main start");
        auto f = std::make_shared<zlynx::Fiber>([]
        {
            ZLYNX_LOG_INFO("fiber start");
            zlynx::Fiber::get_fiber()->yield();
            ZLYNX_LOG_INFO("fiber resume");
            throw std::runtime_error("test exception in fiber");
        });
        zlynx::Fiber::get_fiber();
        f->resume();
        ZLYNX_LOG_INFO("after first resume");
        f->resume();
        ZLYNX_LOG_INFO("main end");
    }
    catch (const std::exception &e)
    {
        ZLYNX_LOG_FATAL("Unhandled exception: {}", e.what());
    }
}
int main()
{
    test1();
    return 0;
}
