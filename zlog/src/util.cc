/**
 * @file util.cc
 * @brief util 实现。
 * @author hzh-betty
 */

#include "zlog/internal/util.h"


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
            if (!exists(pathname)) {
                make_dir(pathname);
            }
            break;
        }
        std::string parent_path = pathname.substr(0, pos + 1);
        if (!exists(parent_path)) {
            // 应该创建父路径而不是原始路径
            make_dir(parent_path);
        }
        index = pos + 1;
    }
}

void File::make_dir(const std::string &pathname) {
    mkdir(pathname.c_str(), 0777);
}

} // namespace zlog
