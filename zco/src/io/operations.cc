#include "zco/io/operations.h"
#include "io/resource.h"
#include <algorithm>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>

namespace zco {
namespace detail {
Result<RegistrationId>
register_io(const std::shared_ptr<Reactor> &reactor,
            const std::shared_ptr<WaitState> &wait,
            const std::shared_ptr<io::detail::Resource> &resource,
            io::Interest interest,
            std::shared_ptr<io::detail::Registration> &registration) {
    if (!resource)
        return Result<RegistrationId>(wait_error(WaitOutcome::closed));
    std::lock_guard<std::mutex> lock(resource->mutex);
    if (resource->fd < 0)
        return Result<RegistrationId>(wait_error(WaitOutcome::closed));
    auto &entries = resource->registrations;
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                       [](const std::weak_ptr<io::detail::Registration> &w) {
                           auto r = w.lock();
                           auto s = r ? r->wait.lock() : nullptr;
                           return !s || s->completed();
                       }),
        entries.end());
    for (auto &weak : entries) {
        auto r = weak.lock();
        if (r && (static_cast<unsigned>(r->interest) &
                  static_cast<unsigned>(interest)))
            return Result<RegistrationId>(
                std::make_error_code(std::errc::device_or_resource_busy));
    }
    auto r = std::make_shared<io::detail::Registration>();
    entries.push_back(r);
    Result<RegistrationId> result(std::make_error_code(std::errc::io_error));
    try {
        result = reactor->add(resource->id, resource->fd, interest);
    } catch (...) {
        entries.pop_back();
        throw;
    }
    if (!result) {
        entries.pop_back();
        return result;
    }
    r->reactor = reactor;
    r->id = result.value();
    r->interest = interest;
    r->wait = wait;
    registration = std::move(r);
    return result;
}

Result<void>
unregister_io(const std::shared_ptr<Reactor> &reactor,
              const std::shared_ptr<io::detail::Resource> &resource,
              const std::shared_ptr<io::detail::Registration> &registration) {
    std::lock_guard<std::mutex> lock(resource->mutex);
    auto removed = reactor->remove(registration->id);
    auto &entries = resource->registrations;
    entries.erase(std::remove_if(
                      entries.begin(), entries.end(),
                      [&](const std::weak_ptr<io::detail::Registration> &weak) {
                          auto entry = weak.lock();
                          return !entry || entry == registration;
                      }),
                  entries.end());
    return removed;
}
} // namespace detail
} // namespace zco

namespace zco {
namespace io {
Result<void> wait_ready(const Descriptor &descriptor, Interest interest,
                        Deadline deadline) {
    return zco::detail::current_wait_ready(descriptor.resource(), interest,
                                           deadline);
}

namespace {
TransferResult transfer(const std::shared_ptr<detail::Resource> &resource,
                        void *buffer, size_t size, Deadline deadline,
                        bool reading) {
    if (!resource)
        return {0, wait_error(WaitOutcome::closed), false};
    if (!size)
        return {};
    while (true) {
        ssize_t count;
        int error;
        {
            std::lock_guard<std::mutex> lock(resource->mutex);
            if (resource->fd < 0)
                return {0, wait_error(WaitOutcome::closed), false};
            if (!resource->nonblocking)
                return {
                    0, std::make_error_code(std::errc::operation_not_permitted),
                    false};
            // send prevents SIGPIPE for sockets; non-sockets use write.
            if (reading)
                count = ::read(resource->fd, buffer, size);
            else {
                count = ::send(resource->fd, buffer, size, MSG_NOSIGNAL);
                if (count < 0 && errno == ENOTSOCK)
                    count = ::write(resource->fd, buffer, size);
            }
            error = errno;
        }
        if (count >= 0)
            return {static_cast<size_t>(count), {}, reading && count == 0};
        if (error == EINTR) {
            if (deadline.expired(Deadline::Clock::now()))
                return {0, wait_error(WaitOutcome::timeout), false};
            continue;
        }
        if (error != EAGAIN && error != EWOULDBLOCK)
            return {0, std::error_code(error, std::generic_category()), false};
        auto ready = zco::detail::current_wait_ready(
            resource, reading ? Interest::read : Interest::write, deadline);
        if (!ready)
            return {0, ready.error(), false};
    }
}

TransferResult aggregate(const Descriptor &descriptor, void *buffer,
                         size_t size, Deadline deadline, bool reading) {
    auto resource = descriptor.resource();
    TransferResult total;
    while (total.bytes < size) {
        if (total.bytes && deadline.expired(Deadline::Clock::now())) {
            total.error = wait_error(WaitOutcome::timeout);
            break;
        }
        auto result =
            transfer(resource, static_cast<char *>(buffer) + total.bytes,
                     size - total.bytes, deadline, reading);
        total.bytes += result.bytes;
        total.error = result.error;
        total.eof = result.eof;
        if (result.error || result.eof)
            break;
        if (!result.bytes) {
            total.error = std::make_error_code(std::errc::io_error);
            break;
        }
    }
    return total;
}
} // namespace

TransferResult read_some(const Descriptor &d, void *b, size_t n, Deadline t) {
    return transfer(d.resource(), b, n, t, true);
}

TransferResult read_exact(const Descriptor &d, void *b, size_t n, Deadline t) {
    return aggregate(d, b, n, t, true);
}

TransferResult write_some(const Descriptor &d, const void *b, size_t n,
                          Deadline t) {
    return transfer(d.resource(), const_cast<void *>(b), n, t, false);
}

TransferResult write_all(const Descriptor &d, const void *b, size_t n,
                         Deadline t) {
    return aggregate(d, const_cast<void *>(b), n, t, false);
}

Result<Deadline> socket_deadline(const Descriptor &descriptor, bool reading) {
    auto resource = descriptor.resource();
    if (!resource)
        return Result<Deadline>(wait_error(WaitOutcome::closed));
    std::lock_guard<std::mutex> lock(resource->mutex);
    timeval timeout{};
    socklen_t size = sizeof(timeout);
    if (::getsockopt(resource->fd, SOL_SOCKET,
                     reading ? SO_RCVTIMEO : SO_SNDTIMEO, &timeout, &size) < 0)
        return Result<Deadline>(
            std::error_code(errno, std::generic_category()));
    if (!timeout.tv_sec && !timeout.tv_usec)
        return Result<Deadline>(Deadline{});
    return Result<Deadline>(
        Deadline::after(std::chrono::seconds(timeout.tv_sec) +
                        std::chrono::microseconds(timeout.tv_usec)));
}
} // namespace io
} // namespace zco
