/**
 * @file sink.cc
 * @brief sink 实现。
 * @author hzh-betty
 */

#include "zlog/sink.h"
#include "zlog/internal/util.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace zlog {
namespace {
void write_stream(std::ofstream &stream, const char *data, size_t len) {
    if (len > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::length_error("file log write is too large");
    }
    if (len != 0) {
        if (!data) {
            throw std::invalid_argument("null log data");
        }
        // 按长度写入原始字节，保留内嵌 NUL，流异常直接报告写入失败。
        stream.write(data, static_cast<std::streamsize>(len));
    }
}

bool is_roll_file(const std::string &name, const std::string &prefix) {
    if (name.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    const size_t time_end = prefix.size() + 14;
    if (name.size() <= time_end + 5 || name[time_end] != '-' ||
        name.compare(name.size() - 4, 4, ".log") != 0) {
        return false;
    }
    for (size_t i = prefix.size(); i < name.size() - 4; ++i) {
        if (i != time_end && (name[i] < '0' || name[i] > '9')) {
            return false;
        }
    }
    return true;
}
} // namespace

void StdOutSink::log(const char *data, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (len != 0 && !data) {
        throw std::invalid_argument("null stdout log data");
    }
    if (len != 0 && std::fwrite(data, 1, len, stdout) != len) {
        throw std::system_error(errno ? errno : EIO, std::generic_category(),
                                "cannot write stdout log");
    }
}

void StdOutSink::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (std::fflush(stdout) != 0) {
        throw std::system_error(errno, std::generic_category(), "cannot flush stdout log");
    }
}

FileSink::FileSink(std::string pathname, bool auto_flush)
    : pathname_(std::move(pathname)), auto_flush_(auto_flush) {
    File::create_directory(File::path(pathname_));
    // 沿用无标准库缓冲的写入策略；打开、写入及刷新失败均抛出异常。
    ofs_.rdbuf()->pubsetbuf(nullptr, 0);
    ofs_.exceptions(std::ios::failbit | std::ios::badbit);
    ofs_.open(pathname_, std::ios::binary | std::ios::app);
}

void FileSink::log(const char *data, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    write_stream(ofs_, data, len);
    // 只在启用 autoFlush 时逐条刷新；否则由公开 flush()/close() 刷新。
    if (auto_flush_) {
        ofs_.flush();
    }
}

void FileSink::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    ofs_.flush();
}

RollBySizeSink::RollBySizeSink(std::string basename, size_t max_size,
                               bool auto_flush, size_t max_files)
    : basename_(std::move(basename)), max_size_(max_size), cur_size_(0),
      name_count_(0), auto_flush_(auto_flush), max_files_(max_files) {
    if (max_size_ == 0 || max_files_ == 0) {
        throw std::invalid_argument("rolling size and file count must be positive");
    }
    // 1. 创建日志文件所用的路径。
    File::create_directory(File::path(basename_));
    ofs_.rdbuf()->pubsetbuf(nullptr, 0);
    ofs_.exceptions(std::ios::failbit | std::ios::badbit);
    // 2. 创建并打开日志文件，清理超出保留数量的旧文件。
    roll_over();
}

void RollBySizeSink::log(const char *data, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    // 超大单条日志在滚动前拒绝，既不拆分记录，也不产生空文件。
    if (len > max_size_) {
        throw std::length_error("log message exceeds rolling file size");
    }
    if (len != 0 && !data) {
        throw std::invalid_argument("null rolling log data");
    }
    if (len > max_size_ - cur_size_) {
        roll_over();
    }
    write_stream(ofs_, data, len);
    if (auto_flush_) {
        ofs_.flush();
    }
    cur_size_ += len;
}

void RollBySizeSink::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    ofs_.flush();
}

std::string RollBySizeSink::create_new_file() {
    const time_t now = std::time(nullptr);
    struct tm local{};
    if (!localtime_r(&now, &local)) {
        throw std::runtime_error("cannot convert rolling timestamp");
    }
    // 先将时间格式化为字符串。
    char time_str[64];
    if (strftime(time_str, sizeof(time_str), "%Y%m%d%H%M%S", &local) == 0) {
        throw std::runtime_error("cannot format rolling timestamp");
    }
    for (;;) {
        const std::string pathname =
            fmt::format("{}_{}-{}.log", basename_, time_str, name_count_++);
        // 独占创建，避免同秒重建、多个实例或进程复用已有文件名。
        const int fd = ::open(pathname.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (fd >= 0) {
            if (::close(fd) != 0) {
                const int error = errno;
                ::unlink(pathname.c_str());
                throw std::system_error(error, std::generic_category(), "cannot close new log file");
            }
            return pathname;
        }
        if (errno != EEXIST) {
            throw std::system_error(errno, std::generic_category(), "cannot create rolling log file");
        }
    }
}

void RollBySizeSink::roll_over() {
    if (ofs_.is_open()) {
        ofs_.flush();
        ofs_.close(); // 释放旧流资源
    }
    const std::string pathname = create_new_file();
    try {
        ofs_.open(pathname, std::ios::binary | std::ios::app);
        pathname_ = pathname;
        cur_size_ = 0;
        prune_files();
    } catch (...) {
        // 打开或清理失败时撤销本次新文件，避免失败的轮转额外留下空文件。
        if (ofs_.is_open()) {
            try {
                ofs_.close();
            } catch (...) {
            }
        }
        ::unlink(pathname.c_str());
        throw;
    }
}

void RollBySizeSink::prune_files() {
    const std::string directory = File::path(basename_);
    const std::string prefix = basename_.substr(directory == "." ? 0 : directory.size()) + "_";
    DIR *raw = opendir(directory.c_str());
    if (!raw) {
        throw std::system_error(errno, std::generic_category(), "cannot list rolling log files");
    }
    const std::unique_ptr<DIR, int (*)(DIR *)> entries(raw, closedir);
    struct Entry {
        std::string path;
        struct timespec modified;
    };
    std::vector<Entry> files;
    errno = 0;
    while (const dirent *entry = readdir(entries.get())) {
        if (is_roll_file(entry->d_name, prefix)) {
            const std::string path = directory + "/" + entry->d_name;
            struct stat st{};
            if (stat(path.c_str(), &st) != 0) {
                if (errno != ENOENT) {
                    throw std::system_error(errno, std::generic_category(), "cannot stat rolling log file");
                }
            } else if (S_ISREG(st.st_mode)) {
                files.push_back({path, st.st_mtim});
            }
        }
        errno = 0;
    }
    if (errno != 0) {
        throw std::system_error(errno, std::generic_category(), "cannot read rolling log directory");
    }
    std::sort(files.begin(), files.end(), [](const Entry &a, const Entry &b) {
        if (a.modified.tv_sec != b.modified.tv_sec) {
            return a.modified.tv_sec < b.modified.tv_sec;
        }
        if (a.modified.tv_nsec != b.modified.tv_nsec) {
            return a.modified.tv_nsec < b.modified.tv_nsec;
        }
        return a.path < b.path;
    });
    size_t count = files.size();
    for (const auto &file : files) {
        // 扫描范围已限定在同一目录，按文件名保护当前文件。
        if (file.path.substr(file.path.find_last_of('/') + 1) ==
            pathname_.substr(pathname_.find_last_of('/') + 1)) {
            continue;
        }
        if (count <= max_files_) {
            break;
        }
        if (::unlink(file.path.c_str()) != 0 && errno != ENOENT) {
            throw std::system_error(errno, std::generic_category(), "cannot remove old log file");
        }
        --count;
    }
}

} // namespace zlog
