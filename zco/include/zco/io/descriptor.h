#pragma once
#include "zco/result.h"
#include <cstdint>
#include <memory>
#include <mutex>

namespace zco {
namespace io {
namespace detail {
struct Resource;
}

struct ResourceId {
    uint64_t value = 0;
};

class Descriptor {
  public:
    class Borrow {
      public:
        explicit Borrow(std::shared_ptr<detail::Resource> resource);
        int fd() const;

      private:
        std::shared_ptr<detail::Resource> resource_;
        std::unique_lock<std::mutex> lock_;
    };

    // Native calls must preserve O_NONBLOCK on this fd and every alias.
    Borrow borrow() const { return Borrow(resource()); }

    Descriptor() = default;
    // Adopts unique ownership. fd must be nonblocking for coroutine operations.
    // O_NONBLOCK is checked here and must remain unchanged until close,
    // including through duplicated fds sharing the open file description.
    explicit Descriptor(int fd);
    ~Descriptor();
    Descriptor(Descriptor &&) noexcept;
    Descriptor &operator=(Descriptor &&) noexcept;
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    // Borrow only: callers must not close/dup2/dup3 over the handle, or change
    // O_NONBLOCK through it or any alias.
    int native_handle() const;
    ResourceId id() const;
    Result<void> close();
    Result<Descriptor> duplicate() const;
    // Replaces this resource, first revoking all old registrations.
    Result<void> replace_with_duplicate(const Descriptor &source);

    std::shared_ptr<detail::Resource> resource() const {
        return std::atomic_load(&resource_);
    }

  private:
    std::shared_ptr<detail::Resource> resource_;
};
} // namespace io
} // namespace zco
