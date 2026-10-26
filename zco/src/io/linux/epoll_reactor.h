#ifndef ZLYNX_ZCO_SRC_IO_LINUX_EPOLL_REACTOR_H_
#define ZLYNX_ZCO_SRC_IO_LINUX_EPOLL_REACTOR_H_

#include "io/reactor.h"
#include <map>
#include <mutex>

namespace zco {
namespace detail {
class EpollReactor final : public Reactor {
  public:
    EpollReactor();
    ~EpollReactor() override;
    Result<RegistrationId> add(io::ResourceId, int, io::Interest) override;
    Result<void> remove(RegistrationId) override;
    std::vector<ReadyEvent> poll(int timeout_ms) override;
    void wake() override;

  private:
    struct Bucket {
        int fd;
        std::map<RegistrationId, unsigned> registrations; // 该资源上所有登记的等待及其兴趣掩码
    };

    int epoll_ = -1, event_ = -1;
    std::mutex mutex_;

    /*
      resources_
        ResourceId=1001
          fd=12
          registrations:
            501 → read
            502 → write

      registrations_
        RegistrationId
          501 → 1001
          502 → 1001
    */
    std::map<uint64_t, Bucket> resources_;  
    std::map<RegistrationId, uint64_t> registrations_;  // 
};
} // namespace detail
} // namespace zco

#endif
