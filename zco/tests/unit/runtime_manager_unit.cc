#include <atomic>
#include <cstdint>
#include <errno.h>
#include <memory>
#include <random>
#include <sys/epoll.h>
#include <thread>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>

#include "support/test_fixture.h"
#include "zco/internal/runtime_manager.h"

namespace zco {
namespace {

class RuntimeManagerUnitTest : public test::RuntimeTestBase {};

TEST_F(RuntimeManagerUnitTest, DelayedStartByGoWorks) {
    Runtime &runtime = Runtime::instance();
    runtime.shutdown();
    EXPECT_EQ(runtime.scheduler_count(), 0u);

    WaitGroup wait_group(1);
    std::atomic<bool> ran(false);

    go([&ran, &wait_group]() {
        ran.store(true, std::memory_order_release);
        wait_group.done();
    });

    wait_group.wait();
    EXPECT_TRUE(ran.load(std::memory_order_acquire));
    EXPECT_GE(runtime.scheduler_count(), 1u);
}

TEST_F(RuntimeManagerUnitTest, SubmitNullTaskIsIgnored) {
    Runtime &runtime = Runtime::instance();
    runtime.init(2);

    runtime.submit(Task());

    WaitGroup wait_group(1);
    std::atomic<int> counter(0);
    runtime.submit([&counter, &wait_group]() {
        counter.fetch_add(1, std::memory_order_relaxed);
        wait_group.done();
    });

    wait_group.wait();
    EXPECT_EQ(counter.load(std::memory_order_relaxed), 1);
}

TEST_F(RuntimeManagerUnitTest, SubmitToUsesModuloSchedulerIndex) {
    Runtime &runtime = Runtime::instance();
    runtime.init(3);

    WaitGroup wait_group(1);
    std::atomic<int> scheduler_id(-1);
    runtime.submit_to(1000, [&scheduler_id, &wait_group]() {
        scheduler_id.store(sched_id(), std::memory_order_release);
        wait_group.done();
    });

    wait_group.wait();
    const int id = scheduler_id.load(std::memory_order_acquire);
    ASSERT_GE(id, 0);
    EXPECT_LT(static_cast<size_t>(id), runtime.scheduler_count());
}

TEST_F(RuntimeManagerUnitTest, MainAndNextSchedulerProvideReusableHandles) {
    Runtime &runtime = Runtime::instance();
    runtime.init(3);

    Scheduler *main_a = runtime.main_scheduler();
    Scheduler *main_b = runtime.main_scheduler();
    ASSERT_NE(main_a, nullptr);
    ASSERT_NE(main_b, nullptr);
    EXPECT_EQ(main_a, main_b);

    std::unordered_set<Scheduler *> handles;
    handles.insert(main_a);
    for (int i = 0; i < 32; ++i) {
        Scheduler *next = runtime.next_scheduler();
        ASSERT_NE(next, nullptr);
        handles.insert(next);
    }

    EXPECT_GE(handles.size(), 2u);
    EXPECT_LE(handles.size(), runtime.scheduler_count());
}

TEST_F(RuntimeManagerUnitTest, SubmitToNullTaskIsIgnored) {
    Runtime &runtime = Runtime::instance();
    runtime.init(2);

    runtime.submit_to(7, Task());

    WaitGroup done(1);
    std::atomic<int> counter(0);
    runtime.submit_to(7, [&counter, &done]() {
        counter.fetch_add(1, std::memory_order_relaxed);
        done.done();
    });

    done.wait();
    EXPECT_EQ(counter.load(std::memory_order_relaxed), 1);
}

TEST_F(RuntimeManagerUnitTest,
       PickSecondaryIndexAvoidsFirstWhenMultipleSchedulers) {
    Runtime &runtime = Runtime::instance();
    runtime.init(4);

    const size_t count = runtime.scheduler_count();
    ASSERT_GE(count, 2u);

    for (uint64_t ticket = 1; ticket < 32; ++ticket) {
        for (size_t first = 0; first < count; ++first) {
            const size_t second = runtime.pick_secondary_index(first, ticket);
            EXPECT_LT(second, count);
            EXPECT_NE(second, first);
        }
    }
}

TEST_F(RuntimeManagerUnitTest,
       PickProcessorIndexReturnsZeroWithSingleScheduler) {
    Runtime &runtime = Runtime::instance();
    runtime.init(1);

    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(runtime.pick_processor_index(), 0u);
    }
}

TEST_F(RuntimeManagerUnitTest,
       PickSecondaryIndexReturnsZeroWithSingleScheduler) {
    Runtime &runtime = Runtime::instance();
    runtime.init(1);

    for (uint64_t ticket = 0; ticket < 8; ++ticket) {
        EXPECT_EQ(runtime.pick_secondary_index(0, ticket), 0u);
    }
}

TEST_F(RuntimeManagerUnitTest, ResumeExternalNullHandleIsNoOp) {
    Runtime &runtime = Runtime::instance();
    runtime.init(1);
    runtime.resume_external(nullptr);

    WaitGroup done(1);
    std::atomic<bool> ran(false);
    runtime.submit([&done, &ran]() {
        ran.store(true, std::memory_order_release);
        done.done();
    });

    done.wait();
    EXPECT_TRUE(ran.load(std::memory_order_acquire));
}

TEST_F(RuntimeManagerUnitTest, NextFiberIdIsMonotonic) {
    Runtime &runtime = Runtime::instance();
    runtime.init(1);

    const int first = runtime.next_fiber_id();
    const int second = runtime.next_fiber_id();
    const int third = runtime.next_fiber_id();

    EXPECT_LT(first, second);
    EXPECT_LT(second, third);
}

TEST_F(RuntimeManagerUnitTest, StackConfigGettersReflectPreStartOverrides) {
    Runtime &runtime = Runtime::instance();
    runtime.shutdown();

    EXPECT_TRUE(runtime.set_stack_num(6));
    EXPECT_TRUE(runtime.set_stack_size(96 * 1024));
    EXPECT_TRUE(runtime.set_stack_model(StackModel::kIndependent));

    EXPECT_EQ(runtime.stack_num(), 6u);
    EXPECT_EQ(runtime.stack_size(), 96u * 1024u);
    EXPECT_EQ(runtime.stack_model(), StackModel::kIndependent);
}

TEST_F(RuntimeManagerUnitTest, NullFiberRegistrationAndHandleApisAreNoop) {
    Runtime &runtime = Runtime::instance();
    runtime.init(1);

    runtime.register_fiber(Fiber::ptr());
    runtime.unregister_fiber(nullptr);
    EXPECT_EQ(runtime.external_handle(Fiber::ptr()), nullptr);
}

TEST_F(RuntimeManagerUnitTest, RegisterFiberAndExternalHandleRoundTrip) {
    Runtime &runtime = Runtime::instance();
    runtime.init(1);

    Processor owner(77, 64 * 1024);
    Fiber::ptr fiber = std::make_shared<Fiber>(1001, &owner, []() {},
                                               64 * 1024, 0, true);
    ASSERT_NE(fiber, nullptr);
    EXPECT_EQ(fiber->external_handle_id(), 0u);

    runtime.register_fiber(fiber);
    const uint64_t first_id = fiber->external_handle_id();
    EXPECT_NE(first_id, 0u);

    void *handle = runtime.external_handle(fiber);
    ASSERT_NE(handle, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(handle),
              static_cast<uintptr_t>(first_id));

    runtime.register_fiber(fiber);
    EXPECT_EQ(fiber->external_handle_id(), first_id);

    fiber->mark_waiting();
    runtime.resume_external(handle);
    EXPECT_EQ(fiber->state(), Fiber::State::kReady);
    EXPECT_FALSE(fiber->timed_out());

    runtime.unregister_fiber(fiber.get());
    EXPECT_EQ(fiber->external_handle_id(), 0u);
}

TEST_F(RuntimeManagerUnitTest, ThreadContextRuntimeHelpersReturnSafeDefaults) {
    Runtime &runtime = Runtime::instance();
    runtime.init(1);

    prepare_current_wait();
    EXPECT_FALSE(park_current());
    EXPECT_FALSE(park_current_for(1));
    EXPECT_EQ(add_timer(1, []() {}), nullptr);

    errno = 0;
    EXPECT_FALSE(wait_fd(0, EPOLLIN, 1));
    EXPECT_EQ(errno, EPERM);
}

TEST_F(RuntimeManagerUnitTest, ResumeFiberNullInputIsNoOp) {
    resume_fiber(Fiber::ptr(), false);

    Runtime &runtime = Runtime::instance();
    runtime.init(1);
    WaitGroup done(1);
    std::atomic<int> counter(0);
    runtime.submit([&counter, &done]() {
        counter.fetch_add(1, std::memory_order_relaxed);
        done.done();
    });
    done.wait();
    EXPECT_EQ(counter.load(std::memory_order_relaxed), 1);
}

TEST_F(RuntimeManagerUnitTest, StackConfigDefaultsToSharedMode) {
    Runtime &runtime = Runtime::instance();
    runtime.init(1);

    const auto &processors = runtime.processors();
    ASSERT_EQ(processors.size(), 1u);
    ASSERT_NE(processors[0], nullptr);

    Processor *processor = processors[0].get();
    EXPECT_EQ(processor->stack_model(), StackModel::kShared);
    EXPECT_EQ(processor->shared_stack_count(), 8u);
    EXPECT_EQ(processor->shared_stack_size(0), 128u * 1024u);
}

TEST_F(RuntimeManagerUnitTest,
       InvalidZeroStackConfigIsRejectedAndDefaultsRemain) {
    Runtime &runtime = Runtime::instance();
    runtime.shutdown();

    EXPECT_FALSE(runtime.set_stack_num(0));
    EXPECT_FALSE(runtime.set_stack_size(0));

    runtime.init(1);
    const auto &processors = runtime.processors();
    ASSERT_EQ(processors.size(), 1u);
    ASSERT_NE(processors[0], nullptr);

    Processor *processor = processors[0].get();
    EXPECT_EQ(processor->stack_model(), StackModel::kShared);
    EXPECT_EQ(processor->shared_stack_count(), 8u);
    EXPECT_EQ(processor->shared_stack_size(0), 128u * 1024u);
}

TEST_F(RuntimeManagerUnitTest, StackConfigSettersAreRejectedAfterRuntimeStart) {
    Runtime &runtime = Runtime::instance();
    runtime.init(1);

    EXPECT_FALSE(runtime.set_stack_num(4));
    EXPECT_FALSE(runtime.set_stack_size(64 * 1024));
    EXPECT_FALSE(runtime.set_stack_model(StackModel::kIndependent));

    const auto &processors = runtime.processors();
    ASSERT_EQ(processors.size(), 1u);
    ASSERT_NE(processors[0], nullptr);
    EXPECT_EQ(processors[0]->stack_model(), StackModel::kShared);
}

TEST_F(RuntimeManagerUnitTest, StackConfigAppliesWhenSetBeforeRuntimeStart) {
    co_stack_num(4);
    co_stack_size(64 * 1024);
    co_stack_model(StackModel::kShared);

    Runtime &runtime = Runtime::instance();
    runtime.init(1);

    const auto &processors = runtime.processors();
    ASSERT_EQ(processors.size(), 1u);
    ASSERT_NE(processors[0], nullptr);

    Processor *processor = processors[0].get();
    EXPECT_EQ(processor->stack_model(), StackModel::kShared);
    EXPECT_EQ(processor->shared_stack_count(), 4u);
    EXPECT_EQ(processor->shared_stack_size(0), 64u * 1024u);
}

TEST_F(RuntimeManagerUnitTest, StackConfigAfterRuntimeStartDoesNotTakeEffect) {
    co_stack_num(2);
    co_stack_size(64 * 1024);
    co_stack_model(StackModel::kShared);

    Runtime &runtime = Runtime::instance();
    runtime.init(1);

    const auto &processors = runtime.processors();
    ASSERT_EQ(processors.size(), 1u);
    ASSERT_NE(processors[0], nullptr);

    Processor *processor = processors[0].get();
    const size_t expected_num = processor->shared_stack_count();
    const size_t expected_size = processor->shared_stack_size(0);
    const StackModel expected_model = processor->stack_model();

    co_stack_num(16);
    co_stack_size(512 * 1024);
    co_stack_model(StackModel::kIndependent);

    EXPECT_EQ(processor->shared_stack_count(), expected_num);
    EXPECT_EQ(processor->shared_stack_size(0), expected_size);
    EXPECT_EQ(processor->stack_model(), expected_model);
}

TEST_F(RuntimeManagerUnitTest, IndependentStackModeRunsWithoutSharedStackPool) {
    co_stack_model(StackModel::kIndependent);
    co_stack_size(64 * 1024);
    co_stack_num(4);

    Runtime &runtime = Runtime::instance();
    runtime.init(1);

    const auto &processors = runtime.processors();
    ASSERT_EQ(processors.size(), 1u);
    ASSERT_NE(processors[0], nullptr);

    Processor *processor = processors[0].get();
    EXPECT_EQ(processor->stack_model(), StackModel::kIndependent);
    EXPECT_EQ(processor->shared_stack_count(), 0u);
    EXPECT_EQ(processor->shared_stack_data(0), nullptr);
    EXPECT_EQ(processor->shared_stack_size(0), 0u);

    WaitGroup done(1);
    std::atomic<bool> ran(false);
    go([&done, &ran]() {
        ran.store(true, std::memory_order_release);
        done.done();
    });
    done.wait();
    EXPECT_TRUE(ran.load(std::memory_order_acquire));
}

TEST_F(RuntimeManagerUnitTest,
       IndependentStackConfigResetsToDefaultsAfterShutdown) {
    Runtime &runtime = Runtime::instance();

    co_stack_model(StackModel::kIndependent);
    co_stack_size(96 * 1024);
    co_stack_num(5);
    runtime.init(1);

    const auto &first_processors = runtime.processors();
    ASSERT_EQ(first_processors.size(), 1u);
    ASSERT_NE(first_processors[0], nullptr);
    EXPECT_EQ(first_processors[0]->stack_model(), StackModel::kIndependent);
    EXPECT_EQ(first_processors[0]->shared_stack_count(), 0u);

    runtime.shutdown();
    runtime.init(1);

    const auto &second_processors = runtime.processors();
    ASSERT_EQ(second_processors.size(), 1u);
    ASSERT_NE(second_processors[0], nullptr);
    EXPECT_EQ(second_processors[0]->stack_model(), StackModel::kShared);
    EXPECT_EQ(second_processors[0]->shared_stack_count(), 8u);
    EXPECT_EQ(second_processors[0]->shared_stack_size(0), 128u * 1024u);
}

TEST_F(RuntimeManagerUnitTest,
       IndependentStackBlockedCoroutinesHaveDistinctStackAddresses) {
    co_stack_model(StackModel::kIndependent);
    co_stack_size(64 * 1024);
    init(1);

    constexpr int kFiberCount = 20;
    Event gate(true, false);
    WaitGroup ready(kFiberCount);
    WaitGroup done(kFiberCount);

    std::vector<std::atomic<uintptr_t>> stack_addrs(kFiberCount);
    for (int i = 0; i < kFiberCount; ++i) {
        stack_addrs[static_cast<size_t>(i)].store(0u,
                                                  std::memory_order_relaxed);
    }

    for (int i = 0; i < kFiberCount; ++i) {
        go([&ready, &done, &gate, &stack_addrs, i]() {
            volatile int local_marker = i;
            stack_addrs[static_cast<size_t>(i)].store(
                reinterpret_cast<uintptr_t>(&local_marker),
                std::memory_order_release);
            ready.done();
            (void)gate.wait();
            done.done();
        });
    }

    ready.wait();

    std::unordered_set<uintptr_t> unique_addrs;
    for (int i = 0; i < kFiberCount; ++i) {
        const uintptr_t addr =
            stack_addrs[static_cast<size_t>(i)].load(std::memory_order_acquire);
        EXPECT_NE(addr, 0u);
        unique_addrs.insert(addr);
    }
    EXPECT_EQ(unique_addrs.size(), static_cast<size_t>(kFiberCount));

    gate.signal();
    done.wait();
}

TEST_F(RuntimeManagerUnitTest, StaleHandleCannotWakeAnotherCoroutine) {
    init(1);

    WaitGroup first_done(1);
    std::atomic<void *> stale_handle(nullptr);
    go([&first_done, &stale_handle]() {
        stale_handle.store(current_coroutine(), std::memory_order_release);
        first_done.done();
    });
    first_done.wait();

    co_sleep_for(5);

    Event gate(false, false);
    WaitGroup second_ready(1);
    WaitGroup second_done(1);
    std::atomic<void *> second_handle(nullptr);
    std::atomic<bool> resumed(false);

    go([&gate, &second_ready, &second_done, &second_handle, &resumed]() {
        second_handle.store(current_coroutine(), std::memory_order_release);
        second_ready.done();
        (void)gate.wait();
        resumed.store(true, std::memory_order_release);
        second_done.done();
    });

    second_ready.wait();

    void *stale = stale_handle.load(std::memory_order_acquire);
    void *current = second_handle.load(std::memory_order_acquire);
    ASSERT_NE(stale, nullptr);
    ASSERT_NE(current, nullptr);
    ASSERT_NE(stale, current);

    resume(stale);
    co_sleep_for(10);
    EXPECT_FALSE(resumed.load(std::memory_order_acquire));

    gate.signal();
    second_done.wait();
    EXPECT_TRUE(resumed.load(std::memory_order_acquire));
}

TEST_F(RuntimeManagerUnitTest, ExternalHandleIsRegisteredLazily) {
    init(1);

    WaitGroup done(1);
    std::atomic<uint64_t> handle_id_before_export(UINT64_MAX);
    std::atomic<uint64_t> handle_id_after_export(0);
    std::atomic<void *> exported_handle(nullptr);

    go([&done, &handle_id_before_export, &handle_id_after_export,
        &exported_handle]() {
        Fiber::ptr fiber = current_fiber_shared();
        handle_id_before_export.store(fiber ? fiber->external_handle_id()
                                            : UINT64_MAX,
                                      std::memory_order_release);

        void *handle = current_coroutine();
        exported_handle.store(handle, std::memory_order_release);

        handle_id_after_export.store(fiber ? fiber->external_handle_id() : 0,
                                     std::memory_order_release);
        done.done();
    });

    done.wait();

    EXPECT_EQ(handle_id_before_export.load(std::memory_order_acquire), 0u);
    EXPECT_NE(exported_handle.load(std::memory_order_acquire), nullptr);
    EXPECT_NE(handle_id_after_export.load(std::memory_order_acquire), 0u);
}

TEST_F(RuntimeManagerUnitTest,
       RandomizedSubmitAndSchedulerDispatchDeterministicSeed) {
    Runtime &runtime = Runtime::instance();
    runtime.init(4);

    constexpr int kThreadCount = 4;
    constexpr int kTasksPerThread = 400;
    constexpr int kTotal = kThreadCount * kTasksPerThread;

    WaitGroup done(kTotal);
    std::atomic<int> counter(0);
    std::vector<std::atomic<int>> scheduler_hits(runtime.scheduler_count());
    for (size_t i = 0; i < scheduler_hits.size(); ++i) {
        scheduler_hits[i].store(0, std::memory_order_relaxed);
    }

    std::vector<std::thread> producers;
    producers.reserve(kThreadCount);
    for (int t = 0; t < kThreadCount; ++t) {
        producers.emplace_back(
            [&runtime, &done, &counter, &scheduler_hits, t]() {
                std::mt19937 rng(20260329u + static_cast<uint32_t>(t));
                std::uniform_int_distribution<int> mode_dist(0, 2);
                std::uniform_int_distribution<size_t> idx_dist(0, 1024);

                for (int i = 0; i < kTasksPerThread; ++i) {
                    const int mode = mode_dist(rng);
                    const auto task = [&done, &counter, &scheduler_hits]() {
                        const int sid = sched_id();
                        if (sid >= 0 &&
                            static_cast<size_t>(sid) < scheduler_hits.size()) {
                            scheduler_hits[static_cast<size_t>(sid)].fetch_add(
                                1, std::memory_order_relaxed);
                        }
                        counter.fetch_add(1, std::memory_order_relaxed);
                        done.done();
                    };

                    if (mode == 0) {
                        runtime.submit(task);
                    } else if (mode == 1) {
                        runtime.submit_to(idx_dist(rng), task);
                    } else {
                        go(task);
                    }
                }
            });
    }

    for (size_t i = 0; i < producers.size(); ++i) {
        producers[i].join();
    }

    done.wait();
    EXPECT_EQ(counter.load(std::memory_order_relaxed), kTotal);

    int total_hits = 0;
    int active_schedulers = 0;
    for (size_t i = 0; i < scheduler_hits.size(); ++i) {
        const int hits = scheduler_hits[i].load(std::memory_order_relaxed);
        total_hits += hits;
        if (hits > 0) {
            ++active_schedulers;
        }
    }

    EXPECT_EQ(total_hits, kTotal);
    EXPECT_GE(active_schedulers, 2);
}

TEST_F(RuntimeManagerUnitTest, RuntimeInitAndShutdownAreIdempotent) {
    Runtime &runtime = Runtime::instance();
    runtime.shutdown();

    runtime.init(1);
    const size_t first_count = runtime.scheduler_count();
    ASSERT_GE(first_count, 1u);

    runtime.init(4);
    EXPECT_EQ(runtime.scheduler_count(), first_count);

    runtime.shutdown();
    EXPECT_EQ(runtime.scheduler_count(), 0u);

    runtime.shutdown();
    EXPECT_EQ(runtime.scheduler_count(), 0u);
}

TEST_F(RuntimeManagerUnitTest, WaitFdRejectsZeroEventsInsideCoroutine) {
    init(1);

    WaitGroup done(1);
    go([&done]() {
        errno = 0;
        EXPECT_FALSE(wait_fd(-1, 0, 10));
        EXPECT_EQ(errno, EINVAL);
        done.done();
    });
    done.wait();
}

} // namespace
} // namespace zco
