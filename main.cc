#include "zlynx_util/zlynx_logger.h"

int main()
{
    zlynx::Init();
    ZLYNX_LOG_DEBUG("hello world");
    ZLYNX_LOG_INFO("hello world");
    ZLYNX_LOG_WARN("hello world");
    ZLYNX_LOG_ERROR("hello world");
    return 0;
}