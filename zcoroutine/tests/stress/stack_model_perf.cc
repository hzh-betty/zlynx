#include <atomic>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include "zcoroutine/channel.h"
#include "zcoroutine/hook.h"
#include "zcoroutine/sched.h"
#include "zcoroutine/wait_group.h"
#include "support/test_fixture.h"

namespace zcoroutine {
namespace {

class StackModelPerfStressTest : public test::RuntimeTestBase {};

constexpr int kSchedulerCount = 8;
constexpr int kProducerThreads = 8;
constexpr int kSchedulerTasks = 120000;
constexpr int kYieldInterval = 8;
constexpr size_t kStackSize = 64 * 1024;
constexpr size_t kSharedStackNum = 8;

struct ScenarioResult {
  const char* scenario;
  StackModel model;
  int workers;
  int operations;
  double seconds;
  double throughput_ops_per_second;
};

void prepare_runtime(StackModel model) {
  shutdown();
  co_stack_model(model);
  co_stack_size(kStackSize);
  co_stack_num(kSharedStackNum);
  init(kSchedulerCount);
}

ScenarioResult run_scheduler_throughput(StackModel model) {
  prepare_runtime(model);

  WaitGroup done(kSchedulerTasks);
  std::atomic<int> executed(0);

  auto start = std::chrono::steady_clock::now();

  std::vector<std::thread> producers;
  producers.reserve(kProducerThreads);

  const int per_thread = kSchedulerTasks / kProducerThreads;
  for (int t = 0; t < kProducerThreads; ++t) {
    producers.emplace_back([t, per_thread, &done, &executed]() {
      const int begin = t * per_thread;
      const int end = (t == (kProducerThreads - 1)) ? kSchedulerTasks : (begin + per_thread);
      for (int i = begin; i < end; ++i) {
        go([&done, &executed, i]() {
          if ((i % kYieldInterval) == 0) {
            yield();
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
  auto end = std::chrono::steady_clock::now();

  EXPECT_EQ(executed.load(std::memory_order_relaxed), kSchedulerTasks);

  const double seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
  EXPECT_GT(seconds, 0.0);

  shutdown();

  return ScenarioResult{"scheduler_submit", model, kProducerThreads, kSchedulerTasks, seconds,
                        static_cast<double>(kSchedulerTasks) / seconds};
}

ScenarioResult run_channel_throughput(StackModel model) {
  prepare_runtime(model);

  constexpr int kProducerCoroutines = 1;
  constexpr int kConsumerCoroutines = 1;
  constexpr int kTotalMessages = 80000;
  constexpr uint32_t kChannelIoTimeoutMs = 100;

  Channel<int> channel(1024);
  WaitGroup done(kProducerCoroutines + kConsumerCoroutines);

  std::atomic<int> produced(0);
  std::atomic<int> consumed(0);
  std::atomic<bool> producer_ok(true);
  std::atomic<bool> consumer_ok(true);

  auto start = std::chrono::steady_clock::now();

  go([&channel, &done, &produced, &producer_ok]() {
      for (int i = 0; i < kTotalMessages; ++i) {
        if (!channel.write(i)) {
          producer_ok.store(false, std::memory_order_release);
          break;
        }
        produced.fetch_add(1, std::memory_order_relaxed);
        if ((i & 63) == 0) {
          yield();
        }
      }
      channel.close();
      done.done();
  });

  go([&channel, &done, &produced, &consumed, &consumer_ok]() {
      int value = 0;
      while (true) {
        if (!channel.read(value, kChannelIoTimeoutMs)) {
          if (!static_cast<bool>(channel)) {
            break;
          }
          continue;
        }
        const int expected = consumed.load(std::memory_order_relaxed);
        if (value != expected) {
          consumer_ok.store(false, std::memory_order_release);
        }
        consumed.fetch_add(1, std::memory_order_relaxed);
        if ((consumed.load(std::memory_order_relaxed) & 63) == 0) {
          yield();
        }
      }
      done.done();
  });

  done.wait();

  auto end = std::chrono::steady_clock::now();

  EXPECT_TRUE(producer_ok.load(std::memory_order_acquire));
  EXPECT_TRUE(consumer_ok.load(std::memory_order_acquire));
  EXPECT_EQ(produced.load(std::memory_order_relaxed), kTotalMessages);
  EXPECT_EQ(consumed.load(std::memory_order_relaxed), kTotalMessages);

  const double seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
  EXPECT_GT(seconds, 0.0);

  shutdown();

  return ScenarioResult{"channel_spsc", model, kProducerCoroutines + kConsumerCoroutines,
                        kTotalMessages, seconds,
                        static_cast<double>(kTotalMessages) / seconds};
}

ScenarioResult run_timer_throughput(StackModel model) {
  prepare_runtime(model);

  constexpr int kTimerTasks = 60000;
  WaitGroup done(kTimerTasks);
  std::atomic<int> executed(0);

  auto start = std::chrono::steady_clock::now();

  for (int i = 0; i < kTimerTasks; ++i) {
    go([i, &done, &executed]() {
      if ((i & 15) == 0) {
        yield();
      }
      co_sleep_for(0);
      executed.fetch_add(1, std::memory_order_relaxed);
      done.done();
    });
  }

  done.wait();
  auto end = std::chrono::steady_clock::now();

  EXPECT_EQ(executed.load(std::memory_order_relaxed), kTimerTasks);

  const double seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
  EXPECT_GT(seconds, 0.0);

  shutdown();

  return ScenarioResult{"timer_sleep", model, kSchedulerCount, kTimerTasks, seconds,
                        static_cast<double>(kTimerTasks) / seconds};
}

ScenarioResult run_hook_io_throughput(StackModel model) {
  prepare_runtime(model);

  constexpr int kRounds = 40000;

  int stream_pair[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, stream_pair) != 0) {
    ADD_FAILURE() << "socketpair failed for hook perf scenario";
    shutdown();
    return ScenarioResult{"hook_socketpair", model, 2, kRounds, 0.0, 0.0};
  }

  if (::fcntl(stream_pair[0], F_SETFL, ::fcntl(stream_pair[0], F_GETFL, 0) | O_NONBLOCK) != 0 ||
      ::fcntl(stream_pair[1], F_SETFL, ::fcntl(stream_pair[1], F_GETFL, 0) | O_NONBLOCK) != 0) {
    ADD_FAILURE() << "failed to set non-blocking mode for hook perf scenario";
    ::close(stream_pair[0]);
    ::close(stream_pair[1]);
    shutdown();
    return ScenarioResult{"hook_socketpair", model, 2, kRounds, 0.0, 0.0};
  }

  WaitGroup done(2);

  std::atomic<int> verified(0);
  std::atomic<bool> writer_ok(true);
  std::atomic<bool> reader_ok(true);

  auto start = std::chrono::steady_clock::now();

  go([&done, &writer_ok, fd = stream_pair[0]]() {
    for (int i = 0; i < kRounds; ++i) {
      const uint32_t marker = static_cast<uint32_t>(i);
      const ssize_t rc = co_write(fd, &marker, sizeof(marker), 2000);
      if (rc != static_cast<ssize_t>(sizeof(marker))) {
        writer_ok.store(false, std::memory_order_release);
        break;
      }
      if ((i & 127) == 0) {
        yield();
      }
    }
    done.done();
  });

  go([&done, &reader_ok, &verified, fd = stream_pair[1]]() {
    for (int i = 0; i < kRounds; ++i) {
      uint32_t marker = 0;
      const ssize_t rc = co_read(fd, &marker, sizeof(marker), 2000);
      if (rc != static_cast<ssize_t>(sizeof(marker))) {
        reader_ok.store(false, std::memory_order_release);
        break;
      }
      if (marker != static_cast<uint32_t>(i)) {
        reader_ok.store(false, std::memory_order_release);
        break;
      }
      verified.fetch_add(1, std::memory_order_relaxed);
      if ((i & 127) == 0) {
        yield();
      }
    }
    done.done();
  });

  done.wait();
  auto end = std::chrono::steady_clock::now();

  EXPECT_TRUE(writer_ok.load(std::memory_order_acquire));
  EXPECT_TRUE(reader_ok.load(std::memory_order_acquire));
  EXPECT_EQ(verified.load(std::memory_order_relaxed), kRounds);

  ::close(stream_pair[0]);
  ::close(stream_pair[1]);

  const double seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
  EXPECT_GT(seconds, 0.0);

  shutdown();

  return ScenarioResult{"hook_socketpair", model, 2, kRounds, seconds,
                        static_cast<double>(kRounds) / seconds};
}

const char* model_name(StackModel model) {
  return model == StackModel::kShared ? "shared" : "independent";
}

void print_result(const ScenarioResult& result) {
  std::cout << "[zcoroutine-perf] scenario=" << result.scenario
            << " model=" << model_name(result.model)
            << " workers=" << result.workers
            << " operations=" << result.operations
            << " elapsed_s=" << std::fixed << std::setprecision(6) << result.seconds
            << " throughput_ops_per_s=" << std::setprecision(2)
            << result.throughput_ops_per_second << std::endl;
}

TEST_F(StackModelPerfStressTest, CoverSchedulerChannelTimerAndHookIoPerformance) {
  zcoroutine::init_logger(zlog::LogLevel::value::ERROR);
  std::vector<ScenarioResult> results;
  results.push_back(run_scheduler_throughput(StackModel::kShared));
  results.push_back(run_scheduler_throughput(StackModel::kIndependent));
  results.push_back(run_channel_throughput(StackModel::kShared));
  results.push_back(run_timer_throughput(StackModel::kShared));
  results.push_back(run_hook_io_throughput(StackModel::kShared));

  for (size_t i = 0; i < results.size(); ++i) {
    print_result(results[i]);
    EXPECT_GT(results[i].throughput_ops_per_second, 0.0);
  }
}

}  // namespace
}  // namespace zcoroutine