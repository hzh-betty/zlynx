#include "sink.h"

#include "util.h"
namespace zlog {

void StdOutSink::log(const char *data, size_t len) {
  fmt::print(stdout, "{:.{}}", data, len);
}

FileSink::FileSink(std::string pathname, bool autoFlush)
    : pathname_(std::move(pathname)), autoFlush_(autoFlush) {
  File::createDirectory(File::path(pathname_));
  // 使用较大的内部缓冲区提高性能
  ofs_.rdbuf()->pubsetbuf(nullptr, 0); // 禁用标准库缓冲，使用系统缓冲
  ofs_.open(pathname_, std::ios::binary | std::ios::app);
}

void FileSink::log(const char *data, size_t len) {
  fmt::print(ofs_, "{:.{}}", data, len);
  // 只在启用autoFlush时才每次flush，否则依赖系统缓冲
  if (autoFlush_) {
    ofs_.flush();
  }
}

RollBySizeSink::RollBySizeSink(std::string basename, const size_t maxSize,
                               bool autoFlush)
    : basename_(std::move(basename)), maxSize_(maxSize), curSize_(0),
      nameCount_(0), autoFlush_(autoFlush) {
  // 1.创建日志文件所用的路径
  const std::string pathname = createNewFile();
  File::createDirectory(File::path(pathname));
  // 2. 创建并打开日志文件
  ofs_.rdbuf()->pubsetbuf(nullptr, 0); // 禁用标准库缓冲
  ofs_.open(pathname, std::ios::binary | std::ios::app);
}

void RollBySizeSink::log(const char *data, size_t len) {
  if (curSize_ + len > maxSize_) {
    rollOver();
  }
  fmt::print(ofs_, "{:.{}}", data, len);
  if (autoFlush_) {
    ofs_.flush();
  }
  curSize_ += len;
}

std::string RollBySizeSink::createNewFile() {
  time_t t = Date::getCurrentTime();
  struct tm lt {};
  localtime_r(&t, &lt);
  // 先将时间格式化为字符串
  char timeStr[64];
  strftime(timeStr, sizeof(timeStr), "%Y%m%d%H%M%S", &lt);

  std::string pathname =
      fmt::format("{}_{}-{}.log", basename_, timeStr, nameCount_++);

  return pathname;
}

void RollBySizeSink::rollOver() {
  ofs_.close(); // 释放旧流资源
  std::string pathname = createNewFile();
  ofs_.open(pathname, std::ios::binary | std::ios::app);
  curSize_ = 0;
}

} // namespace zlog
