#pragma once
#include "zco/coroutine.h"
#include <condition_variable>
#include <list>
#include <mutex>
#include <vector>

namespace zco {
namespace detail {
class WaitQueue;
class CompletionEndpoint {
  public:
    virtual ~CompletionEndpoint() = default;
    virtual void notify(WaitId id) = 0;
};

class WaitState {
  public:
    explicit WaitState(WaitId id,
                       std::weak_ptr<CompletionEndpoint> endpoint = {})
        : id_(id), endpoint_(std::move(endpoint)) {}

    bool complete(WaitOutcome outcome);
    bool completed() const;
    WaitOutcome outcome() const;
    WaitOutcome wait_thread(Deadline deadline);

    WaitId id() const { return id_; }

  private:
    friend class WaitQueue;
    // Only accessed under the registering queue's predicate mutex.
    WaitQueue *queue_ = nullptr;
    std::list<std::weak_ptr<WaitState>>::iterator queue_entry_;
    WaitId id_;
    std::weak_ptr<CompletionEndpoint> endpoint_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool completed_ = false;
    WaitOutcome outcome_ = WaitOutcome::ready;
};

// Implemented by the runtime's current execution entry; no service lookup.
std::shared_ptr<WaitState> prepare_wait();
WaitOutcome park_wait(const std::shared_ptr<WaitState> &, Deadline);
} // namespace detail
} // namespace zco
