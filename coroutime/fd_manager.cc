#include "fd_manager.h"

#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include<algorithm>

extern "C" {
extern int (*fcntl_f)(int, int, ...);
}

namespace zlynx
{
    FdCtx::FdCtx(const int fd)
        : fd_(fd)
    {
        init();
    }

    FdCtx::~FdCtx()
    = default;

    bool FdCtx::init()
    {
        if (is_init_) return true; // 已初始化


        // 判断文件描述符是否有效
        struct stat st{};
        if (fstat(fd_, &st) == -1)
        {
            is_init_ = false;
            is_socket_ = false;
            return false;
        }
        is_init_ = true;
        is_socket_ = S_ISSOCK(st.st_mode); // 判断是否是套接字

        // 如果是套接字
        if (is_socket_)
        {
            int flags = 0;
            if (fcntl_f) {
                flags = fcntl_f(fd_, F_GETFL, 0);
            } else {
                flags = ::fcntl(fd_, F_GETFL, 0);
            }
            if (!(flags & O_NONBLOCK)) {
                const int newf = flags | O_NONBLOCK;
                if (fcntl_f) {
                    fcntl_f(fd_, F_SETFL, newf);
                } else {
                    ::fcntl(fd_, F_SETFL, newf);
                }
                sys_nonblock_ = true;
            } else {
                sys_nonblock_ = true;
            }
        }
        else
        {
            sys_nonblock_ = false;
        }

        return is_init_;
    }

    void FdCtx::set_timeout(const int type, const uint64_t ms)
    {
        if (type == SO_RCVTIMEO)
        {
            recv_timeout_ = ms;
        }
        else
        {
            send_timeout_ = ms;
        }
    }

    uint64_t FdCtx::get_timeout(const int type) const
    {
        if (type == SO_RCVTIMEO) return recv_timeout_;
        return send_timeout_;
    }

    FdManager::FdManager()
    {
        fd_datas_.resize(64);
    }

    FdCtx::ptr FdManager::get_ctx(int fd, const bool auto_create)
    {
        if (fd < 0)
        {
            return nullptr;
        }

        // 先读锁查找
        {
            RWMutexType::ReadLock lock(mutex_);
            if (static_cast<size_t>(fd) < fd_datas_.size())
            {
                auto &it = fd_datas_[fd];
                return it;
            }
        }

        // 写锁创建
        RWMutexType::WriteLock lock(mutex_);
        if (static_cast<size_t>(fd) >= fd_datas_.size())
        {
            // 以几何增长的方式扩展容量
            const size_t old = fd_datas_.size();
            size_t want = std::max(static_cast<size_t>(fd + 1), old + old / 2);

            if (want <= old)
            {
                want = fd + 1;
            }
            fd_datas_.resize(want);
        }

        // 自动创建
        if (!fd_datas_[fd] && auto_create)
        {
            fd_datas_[fd] = std::make_shared<FdCtx>(fd);
        }
        return fd_datas_[fd];
    }

    void FdManager::delete_ctx(const int fd)
    {
        RWMutexType::WriteLock lock(mutex_);
        if (fd >= 0 && static_cast<size_t>(fd) < fd_datas_.size())
        {
            fd_datas_[fd].reset();
        }
    }
} // namespace zlynx
