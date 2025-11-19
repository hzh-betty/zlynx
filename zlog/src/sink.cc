#include "sink.h"

namespace zlog {

void StdOutSink::log(const char *data, size_t len) {
    fmt::print(stdout, "{:.{}}", data, len);
}

FileSink::FileSink(std::string pathname)
    : pathname_(std::move(pathname))
{
    File::createDirectory(File::path(pathname_));
    ofs_.open(pathname_, std::ios::binary | std::ios::app);
}

void FileSink::log(const char *data, size_t len) {
    fmt::print(ofs_, "{:.{}}", data, len);
    ofs_.flush(); // 确保日志及时写入磁盘
}

RollBySizeSink::RollBySizeSink(std::string basename, const size_t maxSize)
    : basename_(std::move(basename)),
      maxSize_(maxSize),
      curSize_(0),
      nameCount_(0)
{
    // 1.创建日志文件所用的路径
    const std::string pathname = createNewFile();
    File::createDirectory(File::path(pathname));
    // 2. 创建并打开日志文件
    ofs_.open(pathname, std::ios::binary | std::ios::app);
}

void RollBySizeSink::log(const char *data, size_t len) {
    if (curSize_ + len > maxSize_)
    {
        rollOver();
    }
    fmt::print(ofs_, "{:.{}}", data, len);
    ofs_.flush(); // 确保日志及时写入磁盘
    curSize_ += len;
}

std::string RollBySizeSink::createNewFile()
{
    time_t t = Date::getCurrentTime();
    struct tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    // 先将时间格式化为字符串
    char timeStr[64];
    strftime(timeStr, sizeof(timeStr), "%Y%m%d%H%M%S", &lt);

    std::string pathname = fmt::format("{}_{}-{}.log",
                                       basename_, timeStr, nameCount_++);

    return pathname;
}

void RollBySizeSink::rollOver()
{
    ofs_.close(); // 释放旧流资源
    std::string pathname = createNewFile();
    ofs_.open(pathname, std::ios::binary | std::ios::app);
    curSize_ = 0;
}

} // namespace zlog
