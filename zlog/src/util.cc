/**
 * @file util.cc
 * @brief util 实现。
 * @author hzh-betty
 */

#include "zlog/internal/util.h"

#include <cerrno>
#include <system_error>


namespace zlog {

bool File::exists(const std::string &pathname) {
    struct stat st {};
    if (stat(pathname.c_str(), &st) < 0) {
        return false;
    }
    return true;
}

std::string File::path(const std::string &pathname) {
    const size_t pos = pathname.find_last_of("/\\");
    if (pos == std::string::npos)
        return ".";
    return pathname.substr(0, pos + 1);
}

void File::create_directory(const std::string &pathname) {
    // 循环创建目录(mkdir)
    // ./abc/bcd/efg
    size_t pos = 0;
    size_t index = 0;
    while (index < pathname.size()) {
        pos = pathname.find_first_of("/\\", index);
        if (pos == std::string::npos) {
            // 应该创建完整路径而不是原始路径
            make_dir(pathname);
            break;
        }
        std::string parent_path = pathname.substr(0, pos + 1);
        // 应该创建父路径而不是原始路径；已有路径也必须是目录。
        make_dir(parent_path);
        index = pos + 1;
    }
}

void File::make_dir(const std::string &pathname) {
    if (mkdir(pathname.c_str(), 0777) == 0) {
        return;
    }
    const int error = errno;
    struct stat st{};
    // 并发创建同一目录可以成功，已有普通文件不能当成目录使用。
    if (error == EEXIST && stat(pathname.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        return;
    }
    throw std::system_error(error, std::generic_category(), "cannot create log directory: " + pathname);
}

} // namespace zlog
