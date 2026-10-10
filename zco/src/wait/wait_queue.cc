#include "zco/detail/wait_queue.h"
#include "wait/wait_state.h"
#include <iterator>

namespace zco {
namespace detail {
WaitTicket::WaitTicket() : state_(prepare_wait()) {}

WaitOutcome WaitTicket::wait(Deadline deadline) const {
    return park_wait(state_, deadline);
}

WaitQueue::~WaitQueue() {
    for (auto &entry : entries_)
        if (auto state = entry.lock())
            state->queue_ = nullptr;
}

void WaitQueue::erase(Entries::iterator entry) {
    if (cleanup_ == entry)
        ++cleanup_;
    entries_.erase(entry);
}

void WaitQueue::prune_expired() {
    // Bound each pass while reclaiming abandoned registrations on later adds.
    size_t budget = entries_.size() < 4 ? entries_.size() : 4;
    while (budget--) {
        if (cleanup_ == entries_.end())
            cleanup_ = entries_.begin();
        auto entry = cleanup_++;
        if (entry->expired())
            entries_.erase(entry);
    }
}

WaitTicket WaitQueue::add() {
    prune_expired();
    WaitTicket ticket;
    entries_.push_back(ticket.state_);
    ticket.state_->queue_ = this;
    ticket.state_->queue_entry_ = std::prev(entries_.end());
    return ticket;
}

void WaitQueue::remove(const WaitTicket &ticket) {
    auto &state = ticket.state_;
    if (state && state->queue_ == this) {
        erase(state->queue_entry_);
        state->queue_ = nullptr;
    }
}

bool WaitQueue::complete_one(WaitOutcome outcome) {
    while (!entries_.empty()) {
        auto state = entries_.front().lock();
        erase(entries_.begin());
        if (state) {
            state->queue_ = nullptr;
            if (state->complete(outcome))
                return true;
        }
    }
    return false;
}

void WaitQueue::complete_all(WaitOutcome outcome) {
    while (complete_one(outcome)) {
    }
}
} // namespace detail
} // namespace zco
