#include <atomic>
#include <random>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "support/test_fixture.h"

namespace zcoroutine {
namespace {

class RuntimeStressTest : public test::RuntimeTestBase {};

TEST_F(RuntimeStressTest, MassiveCoroutineBurst) {
    init(4);

    constexpr int kTaskCount = 4000;
    WaitGroup done(kTaskCount);
    std::atomic<int> counter(0);

    for (int i = 0; i < kTaskCount; ++i) {
        go([&counter, &done]() {
            if ((counter.load(std::memory_order_relaxed) & 7) == 0) {
                yield();
            }
            counter.fetch_add(1, std::memory_order_relaxed);
            done.done();
        });
    }

    done.wait();
    EXPECT_EQ(counter.load(std::memory_order_relaxed), kTaskCount);
}

TEST_F(RuntimeStressTest, MultiThreadSubmitAndRestart) {
    init(4);

    constexpr int kThreadCount = 4;
    constexpr int kTasksPerThread = 200;
    constexpr int kTotal = kThreadCount * kTasksPerThread;

    WaitGroup done(kTotal);
    std::atomic<int> counter(0);

    std::vector<std::thread> producers;
    producers.reserve(kThreadCount);
    for (int t = 0; t < kThreadCount; ++t) {
        producers.emplace_back([&counter, &done]() {
            for (int i = 0; i < kTasksPerThread; ++i) {
                go([&counter, &done]() {
                    counter.fetch_add(1, std::memory_order_relaxed);
                    done.done();
                });
            }
        });
    }

    for (size_t i = 0; i < producers.size(); ++i) {
        producers[i].join();
    }

    done.wait();
    EXPECT_EQ(counter.load(std::memory_order_relaxed), kTotal);

    stop_scheds();
    EXPECT_EQ(scheduler_count(), 0u);

    WaitGroup restart(1);
    go([&restart]() { restart.done(); });
    restart.wait();
    EXPECT_GE(scheduler_count(), 1u);
}

TEST_F(RuntimeStressTest, EightThreadsHundredThousandTasks) {
    init(8);

    constexpr int kThreadCount = 8;
    constexpr int kTotalTasks = 100000;
    constexpr int kTasksPerThread = kTotalTasks / kThreadCount;

    WaitGroup done(kTotalTasks);
    std::atomic<int> executed(0);
    std::atomic<long long> id_sum(0);

    std::vector<std::thread> producers;
    producers.reserve(kThreadCount);
    for (int t = 0; t < kThreadCount; ++t) {
        producers.emplace_back([&done, &executed, &id_sum, t]() {
            const int base = t * kTasksPerThread;
            for (int i = 0; i < kTasksPerThread; ++i) {
                const int id = base + i;
                go([&done, &executed, &id_sum, id]() {
                    if ((id & 15) == 0) {
                        yield();
                    }
                    executed.fetch_add(1, std::memory_order_relaxed);
                    id_sum.fetch_add(id, std::memory_order_relaxed);
                    done.done();
                });
            }
        });
    }

    for (size_t i = 0; i < producers.size(); ++i) {
        producers[i].join();
    }

    done.wait();

    const long long expected_sum = (static_cast<long long>(kTotalTasks - 1) *
                                    static_cast<long long>(kTotalTasks)) /
                                   2;
    EXPECT_EQ(executed.load(std::memory_order_relaxed), kTotalTasks);
    EXPECT_EQ(id_sum.load(std::memory_order_relaxed), expected_sum);
}

TEST_F(RuntimeStressTest, IndependentStackEightThreadsHundredThousandTasks) {
    co_stack_model(StackModel::kIndependent);
    co_stack_size(64 * 1024);
    co_stack_num(1);
    init(8);

    constexpr int kThreadCount = 8;
    constexpr int kTotalTasks = 100000;
    constexpr int kTasksPerThread = kTotalTasks / kThreadCount;

    WaitGroup done(kTotalTasks);
    std::atomic<int> executed(0);
    std::atomic<int> stack_corruption(0);

    std::vector<std::thread> producers;
    producers.reserve(kThreadCount);
    for (int t = 0; t < kThreadCount; ++t) {
        producers.emplace_back([&done, &executed, &stack_corruption, t]() {
            const int base = t * kTasksPerThread;
            for (int i = 0; i < kTasksPerThread; ++i) {
                const int id = base + i;
                go([&done, &executed, &stack_corruption, id]() {
                    int stack_probe[16];
                    for (int k = 0; k < 16; ++k) {
                        stack_probe[k] = id + k;
                    }

                    if ((id & 7) == 0) {
                        yield();
                    }

                    bool corrupted = false;
                    for (int k = 0; k < 16; ++k) {
                        if (stack_probe[k] != id + k) {
                            corrupted = true;
                            break;
                        }
                    }
                    if (corrupted) {
                        stack_corruption.fetch_add(1,
                                                   std::memory_order_relaxed);
                    }

                    executed.fetch_add(1, std::memory_order_relaxed);
                    done.done();
                });
            }
        });
    }

    for (size_t i = 0; i < producers.size(); ++i) {
        producers[i].join();
    }

    done.wait();
    EXPECT_EQ(executed.load(std::memory_order_relaxed), kTotalTasks);
    EXPECT_EQ(stack_corruption.load(std::memory_order_relaxed), 0);
}

TEST_F(RuntimeStressTest, RandomizedConcurrentSubmitPatternDeterministicSeed) {
    init(8);

    constexpr int kThreadCount = 8;
    constexpr int kRoundsPerThread = 1500;

    WaitGroup done(kThreadCount * kRoundsPerThread);
    std::atomic<int> counter(0);

    std::vector<std::thread> producers;
    producers.reserve(kThreadCount);
    for (int t = 0; t < kThreadCount; ++t) {
        producers.emplace_back([&done, &counter, t]() {
            std::mt19937 rng(20260329u + static_cast<uint32_t>(t));
            std::uniform_int_distribution<int> action_dist(0, 3);

            for (int i = 0; i < kRoundsPerThread; ++i) {
                const int action = action_dist(rng);
                go([&done, &counter, action]() {
                    if (action == 0) {
                        yield();
                    } else if (action == 1) {
                        co_sleep_for(0);
                    }
                    counter.fetch_add(1, std::memory_order_relaxed);
                    done.done();
                });
            }
        });
    }

    for (size_t i = 0; i < producers.size(); ++i) {
        producers[i].join();
    }

    done.wait();
    EXPECT_EQ(counter.load(std::memory_order_relaxed),
              kThreadCount * kRoundsPerThread);
}

TEST_F(RuntimeStressTest, FiftyRoundsDeterministicConcurrencyStability) {
    init(8);

    constexpr int kRounds = 50;
    constexpr int kThreadCount = 8;
    constexpr int kTasksPerThread = 120;
    constexpr int kPerRoundTotal = kThreadCount * kTasksPerThread;

    std::atomic<int> total_counter(0);

    for (int round = 0; round < kRounds; ++round) {
        WaitGroup done(kPerRoundTotal);

        std::vector<std::thread> producers;
        producers.reserve(kThreadCount);

        for (int t = 0; t < kThreadCount; ++t) {
            producers.emplace_back([&done, &total_counter, round, t]() {
                std::mt19937 rng(20260329u +
                                 static_cast<uint32_t>(round * 97 + t));
                std::uniform_int_distribution<int> action_dist(0, 4);

                for (int i = 0; i < kTasksPerThread; ++i) {
                    const int action = action_dist(rng);
                    go([&done, &total_counter, action]() {
                        if (action == 0) {
                            yield();
                        } else if (action == 1) {
                            co_sleep_for(0);
                        } else if (action == 2) {
                            yield();
                            co_sleep_for(0);
                        }

                        total_counter.fetch_add(1, std::memory_order_relaxed);
                        done.done();
                    });
                }
            });
        }

        for (size_t i = 0; i < producers.size(); ++i) {
            producers[i].join();
        }

        done.wait();
        EXPECT_EQ(total_counter.load(std::memory_order_relaxed),
                  (round + 1) * kPerRoundTotal);
    }

    EXPECT_EQ(total_counter.load(std::memory_order_relaxed),
              kRounds * kPerRoundTotal);
}

} // namespace
} // namespace zcoroutine
