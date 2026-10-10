#pragma once
#include "zco/coroutine.h"
#include <list>

namespace zco {
namespace detail {
// Minimal bridge for public templates. The caller protects the predicate and
// queue with the same mutex, then releases that mutex before ticket.wait().
class WaitTicket {
  public:
    WaitTicket();

    WakeToken token() const { return WakeToken(state_); }

    WaitOutcome wait(Deadline deadline = {}) const;

    explicit WaitTicket(std::shared_ptr<WaitState> state)
        : state_(std::move(state)) {}

  private:
    friend class WaitQueue;
    std::shared_ptr<WaitState> state_;
};

class WaitQueue {
  public:
    WaitQueue() = default;
    ~WaitQueue();
    WaitQueue(const WaitQueue &) = delete;
    WaitQueue &operator=(const WaitQueue &) = delete;

    WaitTicket add();
    void remove(const WaitTicket &ticket);
    bool complete_one(WaitOutcome outcome = WaitOutcome::ready);
    void complete_all(WaitOutcome outcome = WaitOutcome::ready);

  private:
    using Entries = std::list<std::weak_ptr<WaitState>>;
    void erase(Entries::iterator entry);
    void prune_expired();
    Entries entries_;
    Entries::iterator cleanup_ = entries_.end();
};
} // namespace detail
} // namespace zco
