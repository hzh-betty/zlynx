#include "zco/io/descriptor.h"
#include "io/resource.h"
#include <atomic>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace zco {
namespace io {
namespace {
std::atomic<uint64_t> next_resource{0};

bool descriptor_is_nonblocking(int fd) {
    int flags;
    do {
        flags = ::fcntl(fd, F_GETFL);
    } while (flags < 0 && errno == EINTR);
    if (flags < 0)
        throw std::system_error(errno, std::generic_category(), "F_GETFL");
    return flags & O_NONBLOCK;
}
}

detail::Resource::Resource(int descriptor)
    : fd(descriptor), id{++next_resource},
      nonblocking(descriptor_is_nonblocking(descriptor)) {}

Descriptor::Borrow::Borrow(std::shared_ptr<detail::Resource> resource)
    : resource_(std::move(resource)) {
    if (resource_)
        lock_ = std::unique_lock<std::mutex>(resource_->mutex);
}

int Descriptor::Borrow::fd() const { return resource_ ? resource_->fd : -1; }

Descriptor::Descriptor(int fd) {
    if (fd < 0)
        throw std::invalid_argument("Cannot adopt an invalid descriptor");
    try {
        resource_ = std::make_shared<detail::Resource>(fd);
    } catch (...) {
        ::close(fd);
        throw;
    }
}

Descriptor::~Descriptor() { close(); }

Descriptor::Descriptor(Descriptor &&other) noexcept
    : resource_(std::atomic_exchange(&other.resource_,
                                     std::shared_ptr<detail::Resource>{})) {}

Descriptor &Descriptor::operator=(Descriptor &&other) noexcept {
    if (this != &other) {
        close();
        std::atomic_store(&resource_, std::atomic_exchange(
                                          &other.resource_,
                                          std::shared_ptr<detail::Resource>{}));
    }
    return *this;
}

int Descriptor::native_handle() const {
    auto resource = this->resource();
    if (!resource)
        return -1;
    std::lock_guard<std::mutex> lock(resource->mutex);
    return resource->fd;
}

ResourceId Descriptor::id() const {
    auto resource = this->resource();
    return resource ? resource->id : ResourceId{};
}

Result<void> Descriptor::close() {
    auto resource = this->resource();
    if (!resource)
        return {};
    std::lock_guard<std::mutex> lock(resource->mutex);
    if (resource->fd < 0)
        return {};
    std::error_code removal_error;
    for (auto &weak : resource->registrations) {
        if (auto registration = weak.lock()) {
            if (auto reactor = registration->reactor.lock()) {
                auto removed = reactor->remove(registration->id);
                if (!removed && !removal_error)
                    removal_error = removed.error();
            }
            if (auto wait = registration->wait.lock())
                wait->complete(WaitOutcome::closed);
        }
    }
    resource->registrations.clear();
    int fd = resource->fd;
    resource->fd = -1;
    // Linux closes the fd even on EINTR; retry could close a reused descriptor.
    if (::close(fd) != 0)
        return Result<void>(std::error_code(errno, std::generic_category()));
    return Result<void>(removal_error);
}

Result<Descriptor> Descriptor::duplicate() const {
    auto resource = this->resource();
    if (!resource)
        return Result<Descriptor>(wait_error(WaitOutcome::closed));
    std::lock_guard<std::mutex> lock(resource->mutex);
    if (resource->fd < 0)
        return Result<Descriptor>(wait_error(WaitOutcome::closed));
    int fd = ::fcntl(resource->fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0)
        return Result<Descriptor>(
            std::error_code(errno, std::generic_category()));
    return Result<Descriptor>(Descriptor(fd));
}

Result<void> Descriptor::replace_with_duplicate(const Descriptor &source) {
    if (&source == this)
        return {};
    auto duplicate = source.duplicate();
    if (!duplicate)
        return Result<void>(duplicate.error());
    auto closed = close();
    if (!closed)
        return closed;
    *this = std::move(duplicate).value();
    return {};
}
} // namespace io
} // namespace zco
