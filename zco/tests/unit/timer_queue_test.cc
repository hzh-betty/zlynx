#include "support/runtime_fixture.h"
#include "wait/timer_queue.h"
#include "wait/wait_state.h"
using namespace zco;

TEST(TimerQueue, ExplicitTimeEqualDeadlinesCancelAndLargeJump) {
    detail::TimerQueue timers;
    auto now = Deadline::TimePoint{};
    auto first = std::make_shared<detail::WaitState>(WaitId{});
    auto second = std::make_shared<detail::WaitState>(WaitId{});
    auto third = std::make_shared<detail::WaitState>(WaitId{});
    auto at = Deadline::at(now + std::chrono::seconds(4));
    auto canceled = timers.add(at, WakeToken(first));
    timers.add(at, WakeToken(second));
    timers.add(Deadline::at(now + std::chrono::hours(3)), WakeToken(third));
    EXPECT_EQ(timers.next_deadline().time(), at.time());
    timers.expire(now);
    EXPECT_FALSE(second->completed());
    EXPECT_TRUE(timers.cancel(canceled));
    EXPECT_FALSE(timers.cancel(canceled));
    timers.expire(at.time());
    EXPECT_FALSE(first->completed());
    EXPECT_EQ(second->outcome(), WaitOutcome::timeout);
    timers.expire(now + std::chrono::hours(24));
    EXPECT_EQ(third->outcome(), WaitOutcome::timeout);
    EXPECT_EQ(timers.size(), 0u);
    EXPECT_TRUE(timers.next_deadline().is_infinite());
}

TEST(TimerQueue, InfiniteDeadlineHasNoEntryAndDoesNotKeepWaitAlive) {
    detail::TimerQueue timers;
    auto state = std::make_shared<detail::WaitState>(WaitId{});
    std::weak_ptr<detail::WaitState> weak = state;
    EXPECT_EQ(timers.add({}, WakeToken(state)), 0u);
    auto id = timers.add(test::soon(), WakeToken(state));
    state.reset();
    EXPECT_TRUE(weak.expired());
    EXPECT_TRUE(timers.cancel(id));
}
