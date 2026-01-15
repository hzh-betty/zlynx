#include "io/status_table.h"
#include "hook/hook.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

namespace zcoroutine {

SocketStatus::SocketStatus(const int fd) : fd_(fd) { init(); }

SocketStatus::~SocketStatus() = default;

bool SocketStatus::init() {
  if (is_init_)
    return true;

  // 获取文件状态
  struct stat st {};
  if (fstat(fd_, &st) == -1) {
    is_init_ = false;
    is_socket_ = false;
    return false;
  }

  is_init_ = true;
  is_socket_ = S_ISSOCK(st.st_mode); // 判断是否是Socket套接字

  // 如果是套接字
  if (is_socket_) {
    int flags = 0;
    if (fcntl_f) { // 如果有自定义的fcntl函数
      flags = fcntl_f(fd_, F_GETFL, 0);
    } else {
      flags = ::fcntl(fd_, F_GETFL, 0);
    }

    // 设置非阻塞模式
    if (!(flags & O_NONBLOCK)) {
      const int newf = flags | O_NONBLOCK;
      if (fcntl_f) {
        fcntl_f(fd_, F_SETFL, newf);
      } else {
        ::fcntl(fd_, F_SETFL, newf);
      }
    }
    // 保证socket套接字一定是非阻塞的
    sys_nonblock_ = true;
  } else {
    sys_nonblock_ = false;
  }

  return is_init_;
}

void SocketStatus::set_timeout(const int type, const uint64_t ms) {
  if (type == SO_RCVTIMEO) {
    recv_timeout_ = ms;
  } else {
    send_timeout_ = ms;
  }
}

uint64_t SocketStatus::get_timeout(const int type) const {
  if (type == SO_RCVTIMEO)
    return recv_timeout_;
  return send_timeout_;
}

StatusTable::StatusTable() {
  // 预分配较大容量，避免频繁扩容
  // Linux默认软限制约 1024，硬限制约 4096
  fd_datas_.resize(4096);
}

SocketStatus::ptr StatusTable::get(int fd, bool auto_create) {
  if (fd < 0) {
    return nullptr;
  }

  {
    RWMutex::ReadLock lock(mutex_);
    // 先尝试读锁，如果读锁成功，说明没有其他协程会修改fd_datas_，
    // 所以可以避免写锁的开销
    if (static_cast<size_t>(fd) < fd_datas_.size()) {
      auto &it = fd_datas_[fd];
      if (it || !auto_create) {
        return it;
      }
    } else if (!auto_create) {
      return nullptr;
    }
  }

  RWMutex::WriteLock lock(mutex_);

  if (static_cast<size_t>(fd) >= fd_datas_.size()) {
    const size_t old = fd_datas_.size();
    // 计算新的容量
    // 新的容量至少是当前fd+1，或者当前容量的1.5倍
    size_t want = std::max(static_cast<size_t>(fd + 1), old + old / 2);
    if (want <= old) {
      want = static_cast<size_t>(fd + 1); // 则新的容量设为当前fd+1
    }
    fd_datas_.resize(want);
  }

  if (!fd_datas_[fd] && auto_create) {
    fd_datas_[fd] = std::make_shared<SocketStatus>(fd);
  }

  return fd_datas_[fd];
}

void StatusTable::del(const int fd) {
  RWMutex::WriteLock lock(mutex_);
  if (fd >= 0 && static_cast<size_t>(fd) < fd_datas_.size()) {
    fd_datas_[fd].reset();
  }
}

StatusTable* StatusTable::GetInstance() {
  static StatusTable instance;
  return &instance;
}

} // namespace zcoroutine
