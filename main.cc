#include "zlynx_util/zlynx_logger.h"
#include "core/fiber.h"
#include <thread>
#include <chrono>
int main()
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
        });
        f->resume();
        ZLYNX_LOG_INFO("after first resume");
        f->resume();
        ZLYNX_LOG_INFO("main end");
    }
    catch (const std::exception &e)
    {
        ZLYNX_LOG_FATAL("Unhandled exception: {}", e.what());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    return 0;
}
