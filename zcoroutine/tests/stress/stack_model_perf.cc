#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "zcoroutine/sched.h"
#include "zcoroutine/wait_group.h"
#include "support/test_fixture.h"

namespace zcoroutine {
namespace {

class StackModelPerfStressTest : public test::RuntimeTestBase {};

constexpr int kSchedulerCount = 8;
constexpr int kProducerThreads = 8;
constexpr int kTotalTasks = 120000;
constexpr int kYieldInterval = 8;
constexpr size_t kStackSize = 64 * 1024;
constexpr size_t kSharedStackNum = 8;

enum class LaunchStyle {
  kStdFunctionCapture = 0,
  kUnaryCallable = 1,
};

struct ScenarioResult {
  double seconds;
  double tasks_per_second;
};

struct Payload {
  std::atomic<int>* executed;
  WaitGroup* done;
  int id;
  int yield_interval;
};

void run_payload(Payload* payload) {
  if ((payload->id % payload->yield_interval) == 0) {
    yield();
  }
  payload->executed->fetch_add(1, std::memory_order_relaxed);
  payload->done->done();
}

ScenarioResult run_scenario(StackModel model, LaunchStyle style) {
  shutdown();
  co_stack_model(model);
  co_stack_size(kStackSize);
  co_stack_num(kSharedStackNum);
  init(kSchedulerCount);

  WaitGroup done(kTotalTasks);
  std::atomic<int> executed(0);

  std::vector<Payload> payloads;
  if (style == LaunchStyle::kUnaryCallable) {
    payloads.resize(static_cast<size_t>(kTotalTasks));
    for (int i = 0; i < kTotalTasks; ++i) {
      payloads[static_cast<size_t>(i)] = Payload{&executed, &done, i, kYieldInterval};
    }
  }

  auto start = std::chrono::steady_clock::now();

  std::vector<std::thread> producers;
  producers.reserve(kProducerThreads);

  const int per_thread = kTotalTasks / kProducerThreads;
  for (int t = 0; t < kProducerThreads; ++t) {
    producers.emplace_back([t, per_thread, style, &done, &executed, &payloads]() {
      const int begin = t * per_thread;
      const int end = (t == (kProducerThreads - 1)) ? kTotalTasks : (begin + per_thread);
      for (int i = begin; i < end; ++i) {
        if (style == LaunchStyle::kStdFunctionCapture) {
          go([&done, &executed, i]() {
            if ((i % kYieldInterval) == 0) {
              yield();
            }
            executed.fetch_add(1, std::memory_order_relaxed);
            done.done();
          });
        } else {
          go(&run_payload, &payloads[static_cast<size_t>(i)]);
        }
      }
    });
  }

  for (size_t i = 0; i < producers.size(); ++i) {
    producers[i].join();
  }

  done.wait();
  auto end = std::chrono::steady_clock::now();

  EXPECT_EQ(executed.load(std::memory_order_relaxed), kTotalTasks);

  const double seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
  EXPECT_GT(seconds, 0.0);

  return ScenarioResult{seconds, static_cast<double>(kTotalTasks) / seconds};
}

const char* model_name(StackModel model) {
  return model == StackModel::kShared ? "shared" : "independent";
}

const char* style_name(LaunchStyle style) {
  return style == LaunchStyle::kStdFunctionCapture ? "std_function_capture" : "unary_callable";
}

void print_result(StackModel model, LaunchStyle style, const ScenarioResult& result) {
  std::cout << "[stack-model-bench] model=" << model_name(model)
            << " style=" << style_name(style) << " elapsed_s=" << std::fixed
            << std::setprecision(6) << result.seconds << " throughput_tasks_per_s="
            << std::setprecision(2) << result.tasks_per_second << std::endl;
}

TEST_F(StackModelPerfStressTest, CompareStackModelAndLaunchStyleThroughput) {
  const ScenarioResult shared_std = run_scenario(StackModel::kShared, LaunchStyle::kStdFunctionCapture);
  const ScenarioResult independent_std =
      run_scenario(StackModel::kIndependent, LaunchStyle::kStdFunctionCapture);
  const ScenarioResult shared_unary = run_scenario(StackModel::kShared, LaunchStyle::kUnaryCallable);
  const ScenarioResult independent_unary =
      run_scenario(StackModel::kIndependent, LaunchStyle::kUnaryCallable);

  print_result(StackModel::kShared, LaunchStyle::kStdFunctionCapture, shared_std);
  print_result(StackModel::kIndependent, LaunchStyle::kStdFunctionCapture, independent_std);
  print_result(StackModel::kShared, LaunchStyle::kUnaryCallable, shared_unary);
  print_result(StackModel::kIndependent, LaunchStyle::kUnaryCallable, independent_unary);

  EXPECT_GT(shared_std.tasks_per_second, 0.0);
  EXPECT_GT(independent_std.tasks_per_second, 0.0);
  EXPECT_GT(shared_unary.tasks_per_second, 0.0);
  EXPECT_GT(independent_unary.tasks_per_second, 0.0);
}

}  // namespace
}  // namespace zcoroutine