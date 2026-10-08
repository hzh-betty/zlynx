#pragma once
#include "zco/coroutine.h"
#include <map>
#include <unordered_map>

namespace zco {
namespace detail {
using TimerId = uint64_t;

class TimerQueue {
  public:
    TimerId add(Deadline deadline, WakeToken token);
    bool cancel(TimerId id);
    void expire(Deadline::TimePoint now);
    Deadline next_deadline() const;

    size_t size() const { return entries_.size(); }

  private:
    struct Entry {
        TimerId id;
        WakeToken token;
    };

    std::multimap<Deadline::TimePoint, Entry> entries_;
    std::unordered_map<TimerId, decltype(entries_)::iterator> index_;
    TimerId next_ = 0;
};
} // namespace detail
} // namespace zco
