#include "zlog/util.h"

#include <chrono>
#include <thread>

namespace zlog {

void Spinlock::lock_slow() noexcept {
    {
        int spin_count = 1;

        for (;;) {
            // 第一阶段：只读自旋（指数退避）
            for (int i = 0; i < spin_count; ++i) {
                if (!locked_.load(std::memory_order_relaxed)) {
                    // 尝试抢锁
                    if (!locked_.exchange(true, std::memory_order_acquire)) {
                        return;
                    }
                }
                cpu_relax();
            }

            // 指数退避：翻倍自旋次数，但不超过上限
            if (spin_count < kMaxSpinCount) {
                spin_count <<= 1;
            } else {
                // 达到上限，让出CPU
                std::this_thread::yield();
            }
        }
    }
}

time_t Date::get_current_time() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    return time;
}

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
