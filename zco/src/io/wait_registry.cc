#include "io/wait_registry.h"
#include <utility>

namespace zco {
namespace detail {
IoWaitRegistry::Registration::Registration(
    IoWaitRegistry &owner, std::shared_ptr<io::detail::Resource> resource,
    std::shared_ptr<io::detail::Registration> registration)
    : owner_(&owner), resource_(std::move(resource)),
      registration_(std::move(registration)) {}

IoWaitRegistry::Registration::Registration(Registration &&other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      resource_(std::move(other.resource_)),
      registration_(std::move(other.registration_)) {}

IoWaitRegistry::Registration::~Registration() {
    if (owner_)
        (void)finish();
}

Result<void> IoWaitRegistry::Registration::finish() {
    auto *owner = std::exchange(owner_, nullptr);
    if (!owner)
        return {};
    owner->waits_.erase(registration_->id);
    return unregister_io(owner->reactor_, resource_, registration_);
}

Result<IoWaitRegistry::Registration>
IoWaitRegistry::add(const std::shared_ptr<WaitState> &wait,
                    const std::shared_ptr<io::detail::Resource> &resource,
                    io::Interest interest) {
    std::shared_ptr<io::detail::Registration> registration;
    auto result = register_io(reactor_, wait, resource, interest, registration);
    if (!result)
        return Result<Registration>(result.error());
    Registration scope(*this, resource, std::move(registration));
    waits_.emplace(result.value(), wait);
    return Result<Registration>(std::move(scope));
}

void IoWaitRegistry::dispatch(ReadyEvent event) {
    auto found = waits_.find(event.registration);
    if (found != waits_.end())
        if (auto wait = found->second.lock())
            wait->complete(WaitOutcome::ready);
}
} // namespace detail
} // namespace zco
