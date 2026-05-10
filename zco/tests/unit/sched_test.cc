#include <array>
#include <atomic>

#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zco/sched.h"

namespace zco {
namespace {

class SchedUnitByHeaderTest : public test::RuntimeTestBase {};

class IncrementClosure final : public Closure {
  public:
    IncrementClosure(std::atomic<int> *counter, WaitGroup *done)
        : counter_(counter), done_(done) {}

    void run() override {
        counter_->fetch_add(1, std::memory_order_relaxed);
        done_->done();
    }

  private:
    std::atomic<int> *counter_;
    WaitGroup *done_;
};

class MemberInvoker {
  public:
    explicit MemberInvoker(WaitGroup *done) : done_(done) {}

    void add(std::atomic<int> *value) {
        value->fetch_add(1, std::memory_order_relaxed);
        if (done_ != nullptr) {
            done_->done();
        }
    }

  private:
    WaitGroup *done_;
};

TEST_F(SchedUnitByHeaderTest, InitGoAndSchedulerHandlesWork) {
    init(3);

    EXPECT_GE(scheduler_count(), 1u);
    Scheduler *main_scheduler = main_sched();
    Scheduler *next_scheduler = next_sched();
    ASSERT_NE(main_scheduler, nullptr);
    ASSERT_NE(next_scheduler, nullptr);

    WaitGroup done(2);
    std::atomic<int> ran(0);

    main_scheduler->go([&]() {
        ran.fetch_add(1, std::memory_order_relaxed);
        done.done();
    });

    next_scheduler->go([&]() {
        ran.fetch_add(1, std::memory_order_relaxed);
        done.done();
    });

    done.wait();
    EXPECT_EQ(ran.load(std::memory_order_relaxed), 2);
}

TEST_F(SchedUnitByHeaderTest, ResumeNullAndSleepInThreadContextAreSafe) {
    resume(nullptr);
    sleep_for(1);
    EXPECT_EQ(coroutine_id(), -1);
}

TEST_F(SchedUnitByHeaderTest,
       CoroutineContextReportsExpectedIdentifiersAndHandle) {
    init(2);

    WaitGroup done(1);
    std::atomic<bool> inside_coroutine(false);
    std::atomic<int> observed_sched(-1);
    std::atomic<int> observed_cid(-1);
    std::atomic<void *> observed_handle(nullptr);

    go([&done, &inside_coroutine, &observed_sched, &observed_cid,
        &observed_handle]() {
        inside_coroutine.store(in_coroutine(), std::memory_order_release);
        observed_sched.store(sched_id(), std::memory_order_release);
        observed_cid.store(coroutine_id(), std::memory_order_release);
        observed_handle.store(current_coroutine(), std::memory_order_release);
        done.done();
    });

    done.wait();

    EXPECT_TRUE(inside_coroutine.load(std::memory_order_acquire));
    EXPECT_GE(observed_sched.load(std::memory_order_acquire), 0);
    EXPECT_GE(observed_cid.load(std::memory_order_acquire), 0);
    EXPECT_NE(observed_handle.load(std::memory_order_acquire), nullptr);
}

TEST_F(SchedUnitByHeaderTest, ThreadContextYieldAndCurrentCoroutineRemainSafe) {
    EXPECT_FALSE(in_coroutine());
    EXPECT_EQ(current_coroutine(), nullptr);
    EXPECT_EQ(coroutine_id(), -1);

    yield();

    EXPECT_FALSE(in_coroutine());
    EXPECT_EQ(current_coroutine(), nullptr);
    EXPECT_EQ(coroutine_id(), -1);
}

TEST_F(SchedUnitByHeaderTest, GoSupportsClosurePointerAndAutoDelete) {
    init(2);

    WaitGroup done(1);
    std::atomic<int> counter(0);

    go(new IncrementClosure(&counter, &done));

    done.wait();
    EXPECT_EQ(counter.load(std::memory_order_relaxed), 1);
}

TEST_F(SchedUnitByHeaderTest, GoSupportsCallableWithOneArgument) {
    init(2);

    WaitGroup done(1);
    std::atomic<int> counter(0);

    go(
        [&done](std::atomic<int> *value) {
            value->fetch_add(1, std::memory_order_relaxed);
            done.done();
        },
        &counter);

    done.wait();
    EXPECT_EQ(counter.load(std::memory_order_relaxed), 1);
}

TEST_F(SchedUnitByHeaderTest, GoSupportsMemberFunctionWithOneArgument) {
    init(2);

    WaitGroup done(1);
    std::atomic<int> counter(0);
    MemberInvoker invoker(&done);

    go(&MemberInvoker::add, &invoker, &counter);

    done.wait();
    EXPECT_EQ(counter.load(std::memory_order_relaxed), 1);
}

TEST_F(SchedUnitByHeaderTest, SchedulerClosurePointerAndIdWork) {
    init(2);

    Scheduler *scheduler = next_sched();
    ASSERT_NE(scheduler, nullptr);
    EXPECT_GE(scheduler->id(), 0);

    WaitGroup done(1);
    std::atomic<int> counter(0);
    scheduler->go(new IncrementClosure(&counter, &done));

    done.wait();
    EXPECT_EQ(counter.load(std::memory_order_relaxed), 1);
}

TEST_F(SchedUnitByHeaderTest, NullClosurePointersAreIgnored) {
    init(1);

    go(static_cast<Closure *>(nullptr));
    Scheduler *scheduler = main_sched();
    ASSERT_NE(scheduler, nullptr);
    scheduler->go(static_cast<Closure *>(nullptr));

    WaitGroup done(1);
    std::atomic<int> ran(0);
    go([&done, &ran]() {
        ran.fetch_add(1, std::memory_order_relaxed);
        done.done();
    });
    done.wait();
    EXPECT_EQ(ran.load(std::memory_order_relaxed), 1);
}

TEST_F(SchedUnitByHeaderTest, SleepForZeroInCoroutineActsLikeYield) {
    init(1);

    WaitGroup done(2);
    std::atomic<int> counter(0);

    go([&done, &counter]() {
        counter.fetch_add(1, std::memory_order_relaxed);
        sleep_for(0);
        counter.fetch_add(1, std::memory_order_relaxed);
        done.done();
    });

    go([&done, &counter]() {
        counter.fetch_add(1, std::memory_order_relaxed);
        done.done();
    });

    done.wait();
    EXPECT_EQ(counter.load(std::memory_order_relaxed), 3);
}

TEST_F(SchedUnitByHeaderTest,
       SingleSharedStackSlotPreservesLocalDataAcrossYields) {
    co_stack_model(StackModel::kShared);
    co_stack_size(64 * 1024);
    co_stack_num(1);
    init(1);

    WaitGroup done(2);
    std::atomic<bool> valid(true);

    auto run_with_pattern = [&done, &valid](uint8_t pattern) {
        std::array<uint8_t, 4096> local;
        local.fill(pattern);

        for (int round = 0; round < 24; ++round) {
            local[static_cast<size_t>(round)] =
                static_cast<uint8_t>(pattern + round);
            yield();
            for (int i = 0; i <= round; ++i) {
                if (local[static_cast<size_t>(i)] !=
                    static_cast<uint8_t>(pattern + i)) {
                    valid.store(false, std::memory_order_release);
                    break;
                }
            }
            for (size_t i = static_cast<size_t>(round + 1); i < local.size();
                 ++i) {
                if (local[i] != pattern) {
                    valid.store(false, std::memory_order_release);
                    break;
                }
            }
        }

        done.done();
    };

    go([&]() { run_with_pattern(0x31); });
    go([&]() { run_with_pattern(0x73); });

    done.wait();
    EXPECT_TRUE(valid.load(std::memory_order_acquire));
}

TEST_F(SchedUnitByHeaderTest, StopSchedsIsIdempotent) {
    init(1);
    stop_scheds();
    stop_scheds();

    init(1);
    EXPECT_GE(scheduler_count(), 1u);
}

} // namespace
} // namespace zco

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    zco::init_logger();
    return RUN_ALL_TESTS();
}
