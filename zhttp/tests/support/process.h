#pragma once
#include "zco/deadline.h"
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <stdexcept>
#include <sys/wait.h>
#include <system_error>
#include <unistd.h>

namespace zhttp::test {
class ScopedChild {
  public:
    explicit ScopedChild(pid_t pid) : pid_(pid) {}

    ~ScopedChild() {
        if (!reaped_) {
            ::kill(pid_, SIGKILL);
            while (::waitpid(pid_, &status_, 0) < 0 && errno == EINTR) {
            }
        }
    }

    bool exited() {
        if (reaped_)
            return true;
        const auto result = ::waitpid(pid_, &status_, WNOHANG);
        if (result == pid_)
            reaped_ = true;
        else if (result < 0 && errno != EINTR)
            throw std::system_error(errno, std::generic_category(), "waitpid");
        return reaped_;
    }

    bool wait(zco::Deadline deadline) {
        while (!deadline.expired(zco::Deadline::Clock::now())) {
            if (exited())
                return true;
            ::poll(nullptr, 0, 5);
        }
        return exited();
    }

    int status() const { return status_; }

    pid_t pid() const { return pid_; }

  private:
    pid_t pid_;
    bool reaped_ = false;
    int status_ = 0;
};
} // namespace zhttp::test
