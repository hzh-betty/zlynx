#include "zco/detail/wait_queue.h"
#include "wait/wait_state.h"
#include <algorithm>

namespace zco {
namespace detail {
WaitTicket::WaitTicket() : state_(prepare_wait()) {}

WaitOutcome WaitTicket::wait(Deadline deadline) const {
    return park_wait(state_, deadline);
}

WaitTicket WaitQueue::add() {
    WaitTicket ticket;
    entries_.push_back(ticket.state_);
    return ticket;
}

void WaitQueue::remove(const WaitTicket &ticket) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const std::weak_ptr<WaitState> &weak) {
                                      auto state = weak.lock();
                                      return !state || state == ticket.state_;
                                  }),
                   entries_.end());
}

bool WaitQueue::complete_one(WaitOutcome outcome) {
    while (!entries_.empty()) {
        auto state = entries_.front().lock();
        entries_.pop_front();
        if (state && state->complete(outcome))
            return true;
    }
    return false;
}

void WaitQueue::complete_all(WaitOutcome outcome) {
    while (complete_one(outcome)) {
    }
}
} // namespace detail
} // namespace zco
