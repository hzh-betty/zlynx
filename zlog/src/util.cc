#include "util.h"
#include <chrono>

namespace zlog {

time_t Date::getCurrentTime()
{
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    return time;
}

bool File::exists(const std::string &pathname)
{
    struct stat st{};
    if (stat(pathname.c_str(), &st) < 0)
    {
        return false;
    }
    return true;
}

std::string File::path(const std::string &pathname)
{
    const size_t pos = pathname.find_last_of("/\\");
    if (pos == std::string::npos)
        return ".";
    return pathname.substr(0, pos + 1);
}

void File::createDirectory(const std::string &pathname)
{
    // 循环创建目录(mkdir)
    // ./abc/bcd/efg
    size_t pos = 0;
    size_t index = 0;
    while (index < pathname.size())
    {
        pos = pathname.find_first_of("/\\", index);
        if (pos == std::string::npos)
        {
            // 应该创建完整路径而不是原始路径
            if (!exists(pathname))
            {
                makeDir(pathname);
            }
            break;
        }
        std::string parentPath = pathname.substr(0, pos + 1);
        if (!exists(parentPath))
        {
            // 应该创建父路径而不是原始路径
            makeDir(parentPath);
        }
        index = pos + 1;
    }
}

void File::makeDir(const std::string &pathname)
{
#ifdef _WIN32
    _mkdir(pathname.c_str());
#else
    mkdir(pathname.c_str(), 0777);
#endif
}

} // namespace zlog
