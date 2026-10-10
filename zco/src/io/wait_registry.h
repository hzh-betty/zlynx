#pragma once
#include "io/resource.h"
#include <unordered_map>

namespace zco {
namespace detail {
// Accessed only by the owning worker thread. The registry must outlive its
// registrations.
class IoWaitRegistry {
  public:
    class Registration {
      public:
        Registration(Registration &&) noexcept;
        Registration(const Registration &) = delete;
        Registration &operator=(const Registration &) = delete;
        ~Registration();
        Result<void> finish();

      private:
        friend class IoWaitRegistry;
        Registration(IoWaitRegistry &, std::shared_ptr<io::detail::Resource>,
                     std::shared_ptr<io::detail::Registration>);
        IoWaitRegistry *owner_;
        std::shared_ptr<io::detail::Resource> resource_;
        std::shared_ptr<io::detail::Registration> registration_;
    };

    explicit IoWaitRegistry(std::shared_ptr<Reactor> reactor)
        : reactor_(std::move(reactor)) {}

    IoWaitRegistry(const IoWaitRegistry &) = delete;
    IoWaitRegistry &operator=(const IoWaitRegistry &) = delete;

    Result<Registration> add(const std::shared_ptr<WaitState> &,
                             const std::shared_ptr<io::detail::Resource> &,
                             io::Interest);
    void dispatch(ReadyEvent);

  private:
    std::shared_ptr<Reactor> reactor_;
    std::unordered_map<RegistrationId, std::weak_ptr<WaitState>> waits_;
};
} // namespace detail
} // namespace zco
