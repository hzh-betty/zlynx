#include "support/runtime_fixture.h"
#include "wait/wait_state.h"
#include <array>
using namespace zco;

namespace {
struct Endpoint : detail::CompletionEndpoint {
    std::atomic<int> notifications{0};

    void notify(WaitId) override { ++notifications; }
};

TEST(WaitState, CompletesBeforeRegistrationAndBeforeParking) {
    auto endpoint = std::make_shared<Endpoint>();
    auto state =
        std::make_shared<detail::WaitState>(WaitId{TaskId{1}, 1}, endpoint);
    WakeToken token(state);
    EXPECT_TRUE(token.complete());
    EXPECT_FALSE(token.complete(WaitOutcome::timeout));
    EXPECT_EQ(state->wait_thread(test::ms(0)), WaitOutcome::ready);
    EXPECT_EQ(endpoint->notifications, 1);
}

TEST(WaitState, SignalTimeoutCloseAndStopHaveOneWinner) {
    for (int iteration = 0; iteration < 100; ++iteration) {
        auto endpoint = std::make_shared<Endpoint>();
        auto state =
            std::make_shared<detail::WaitState>(WaitId{TaskId{1}, 1}, endpoint);
        WakeToken token(state);
        std::atomic<int> winners{0};
        std::array<std::thread, 4> threads;
        int i = 0;
        for (auto outcome : {WaitOutcome::ready, WaitOutcome::timeout,
                             WaitOutcome::closed, WaitOutcome::canceled})
            threads[i++] = std::thread([&, outcome] {
                if (token.complete(outcome))
                    ++winners;
            });
        for (auto &thread : threads)
            thread.join();
        EXPECT_EQ(winners, 1);
        EXPECT_EQ(endpoint->notifications, 1);
    }
}

TEST(WaitState, ExpiredAndOldTokensCannotCompleteANewGeneration) {
    WakeToken old;
    {
        auto state = std::make_shared<detail::WaitState>(WaitId{TaskId{8}, 1});
        old = WakeToken(state);
        EXPECT_TRUE(old.complete());
    }
    auto next = std::make_shared<detail::WaitState>(WaitId{TaskId{8}, 2});
    EXPECT_FALSE(old.complete());
    EXPECT_FALSE(next->completed());
    EXPECT_TRUE(WakeToken(next).complete(WaitOutcome::closed));
    EXPECT_EQ(next->outcome(), WaitOutcome::closed);
}

TEST(WaitState, ThreadWaitUsesTheSameCompletion) {
    auto state = std::make_shared<detail::WaitState>(WaitId{});
    std::thread thread([&] { state->complete(WaitOutcome::canceled); });
    EXPECT_EQ(state->wait_thread(test::soon()), WaitOutcome::canceled);
    thread.join();
}
} // namespace
