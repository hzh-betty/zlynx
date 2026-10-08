#include "wait/timer_queue.h"

namespace zco {
namespace detail {
TimerId TimerQueue::add(Deadline deadline, WakeToken token) {
    if (deadline.is_infinite())
        return 0;
    auto id = ++next_;
    auto it = entries_.emplace(deadline.time(), Entry{id, std::move(token)});
    try {
        index_.emplace(id, it);
    } catch (...) {
        entries_.erase(it);
        throw;
    }
    return id;
}

bool TimerQueue::cancel(TimerId id) {
    auto it = index_.find(id);
    if (it == index_.end())
        return false;
    entries_.erase(it->second);
    index_.erase(it);
    return true;
}

void TimerQueue::expire(Deadline::TimePoint now) {
    while (!entries_.empty() && entries_.begin()->first <= now) {
        auto entry = std::move(entries_.begin()->second);
        index_.erase(entry.id);
        entries_.erase(entries_.begin());
        entry.token.complete(WaitOutcome::timeout);
    }
}

Deadline TimerQueue::next_deadline() const {
    return entries_.empty() ? Deadline{}
                            : Deadline::at(entries_.begin()->first);
}
} // namespace detail
} // namespace zco
