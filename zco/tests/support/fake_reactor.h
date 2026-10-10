#pragma once
#include "io/reactor.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>

namespace zco {
namespace test {
class FakeReactor final : public detail::Reactor {
  public:
    Result<detail::RegistrationId> add(io::ResourceId resource, int,
                                       io::Interest interest) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fail_add)
            return Result<detail::RegistrationId>(
                std::make_error_code(std::errc::io_error));
        auto id = ++next_;
        entries_.push_back({id, resource.value, interest});
        cv_.notify_all();
        return Result<detail::RegistrationId>(id);
    }

    Result<void> remove(detail::RegistrationId id) override {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.erase(
            std::remove_if(entries_.begin(), entries_.end(),
                           [id](const Entry &e) { return e.id == id; }),
            entries_.end());
        return {};
    }

    std::vector<detail::ReadyEvent> poll(int timeout) override {
        std::unique_lock<std::mutex> lock(mutex_);
        if (timeout < 0)
            cv_.wait(lock, [&] { return woken_; });
        else if (timeout)
            cv_.wait_for(lock, std::chrono::milliseconds(timeout),
                         [&] { return woken_; });
        woken_ = false;
        if (fail_poll_) {
            ++poll_failures;
            throw std::system_error(std::make_error_code(std::errc::io_error));
        }
        std::vector<detail::ReadyEvent> result;
        result.swap(events_);
        return result;
    }

    void wake() override {
        ++wake_calls;
        std::lock_guard<std::mutex> lock(mutex_);
        woken_ = true;
        cv_.notify_all();
    }

    void emit(detail::RegistrationId id, io::Interest interest) {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back({id, interest});
        woken_ = true;
        cv_.notify_all();
    }

    void fail_poll() {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_poll_ = true;
        woken_ = true;
        cv_.notify_all();
    }

    size_t registration_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

    detail::RegistrationId await_registration() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, std::chrono::seconds(3),
                          [&] { return !entries_.empty(); }))
            throw std::runtime_error(
                "Fake reactor registration did not arrive");
        return entries_.back().id;
    }

    bool fail_add = false;
    std::atomic<size_t> wake_calls{0};
    std::atomic<size_t> poll_failures{0};

  private:
    struct Entry {
        detail::RegistrationId id;
        uint64_t resource;
        io::Interest interest;
    };

    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Entry> entries_;
    std::vector<detail::ReadyEvent> events_;
    bool woken_ = false;
    bool fail_poll_ = false;
    uint64_t next_ = 0;
};
} // namespace test
} // namespace zco
