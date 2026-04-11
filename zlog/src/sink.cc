#include "sink.h"

#include "util.h"
namespace zlog {

void StdOutSink::log(const char *data, size_t len) {
    fmt::print(stdout, "{:.{}}", data, len);
}

FileSink::FileSink(std::string pathname, bool auto_flush)
    : pathname_(std::move(pathname)), auto_flush_(auto_flush) {
    File::create_directory(File::path(pathname_));
    // 使用较大的内部缓冲区提高性能
    ofs_.rdbuf()->pubsetbuf(nullptr, 0); // 禁用标准库缓冲，使用系统缓冲
    ofs_.open(pathname_, std::ios::binary | std::ios::app);
}

void FileSink::log(const char *data, size_t len) {
    fmt::print(ofs_, "{:.{}}", data, len);
    // 只在启用autoFlush时才每次flush，否则依赖系统缓冲
    if (auto_flush_) {
        ofs_.flush();
    }
}

RollBySizeSink::RollBySizeSink(std::string basename, const size_t max_size,
                               bool auto_flush)
    : basename_(std::move(basename)), max_size_(max_size), cur_size_(0),
      name_count_(0), auto_flush_(auto_flush) {
    // 1.创建日志文件所用的路径
    const std::string pathname = create_new_file();
    File::create_directory(File::path(pathname));
    // 2. 创建并打开日志文件
    ofs_.rdbuf()->pubsetbuf(nullptr, 0); // 禁用标准库缓冲
    ofs_.open(pathname, std::ios::binary | std::ios::app);
}

void RollBySizeSink::log(const char *data, size_t len) {
    if (cur_size_ + len > max_size_) {
        roll_over();
    }
    fmt::print(ofs_, "{:.{}}", data, len);
    if (auto_flush_) {
        ofs_.flush();
    }
    cur_size_ += len;
}

std::string RollBySizeSink::create_new_file() {
    time_t t = Date::get_current_time();
    struct tm lt {};
    localtime_r(&t, &lt);
    // 先将时间格式化为字符串
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y%m%d%H%M%S", &lt);

    std::string pathname =
        fmt::format("{}_{}-{}.log", basename_, time_str, name_count_++);

    return pathname;
}

void RollBySizeSink::roll_over() {
    ofs_.close(); // 释放旧流资源
    std::string pathname = create_new_file();
    ofs_.open(pathname, std::ios::binary | std::ios::app);
    cur_size_ = 0;
}

} // namespace zlog
