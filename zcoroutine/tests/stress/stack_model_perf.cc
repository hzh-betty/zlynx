#include <atomic>
#include <chrono>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstdint>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "zcoroutine/channel.h"
#include "zcoroutine/hook.h"
#include "zcoroutine/log.h"
#include "zcoroutine/sched.h"
#include "zcoroutine/wait_group.h"

namespace zcoroutine {
namespace {

constexpr int kDefaultSchedulerCount = 8;
constexpr int kDefaultProducerThreads = 8;
constexpr int kDefaultSchedulerTasks = 120000;
constexpr int kDefaultYieldInterval = 8;
constexpr size_t kDefaultStackSize = 64 * 1024;
constexpr size_t kDefaultSharedStackNum = 8;
constexpr int kDefaultChannelMessages = 80000;
constexpr int kDefaultTimerTasks = 60000;
constexpr int kDefaultHookRounds = 40000;

struct WorkloadConfig {
  int scheduler_count;
  int producer_threads;
  int scheduler_tasks;
  int yield_interval;
  size_t stack_size;
  size_t shared_stack_num;
  int channel_messages;
  int timer_tasks;
  int hook_rounds;
};

struct ScenarioResult {
  const char* scenario;
  StackModel model;
  int workers;
  int operations;
  double seconds;
  double throughput_ops_per_second;
};

class RuntimeScenarioGuard {
 public:
  RuntimeScenarioGuard() = default;
  ~RuntimeScenarioGuard() { shutdown(); }
};

class ScopedFd {
 public:
  ScopedFd() : fd_(-1) {}
  explicit ScopedFd(int fd) : fd_(fd) {}

  ~ScopedFd() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;

  ScopedFd(ScopedFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

  ScopedFd& operator=(ScopedFd&& other) noexcept {
    if (this == &other) {
      return *this;
    }

    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = other.fd_;
    other.fd_ = -1;
    return *this;
  }

  int get() const { return fd_; }

 private:
  int fd_;
};

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

void require_true(bool condition, const std::string& message) {
  if (!condition) {
    fail(message);
  }
}

template <typename T, typename U>
void require_eq(const T& actual, const U& expected, const std::string& message) {
  if (!(actual == expected)) {
    std::ostringstream oss;
    oss << message << " actual=" << actual << " expected=" << expected;
    fail(oss.str());
  }
}

void require_positive(double value, const std::string& message) {
  if (value <= 0.0) {
    std::ostringstream oss;
    oss << message << " value=" << value;
    fail(oss.str());
  }
}

int read_env_int(const char* name, int default_value, int min_value, int max_value) {
  const char* raw = std::getenv(name);
  if (!raw || raw[0] == '\0') {
    return default_value;
  }

  char* end = nullptr;
  errno = 0;
  const long parsed = std::strtol(raw, &end, 10);
  if (errno != 0 || end == raw || (end && *end != '\0')) {
    return default_value;
  }

  if (parsed < static_cast<long>(min_value) || parsed > static_cast<long>(max_value)) {
    return default_value;
  }

  return static_cast<int>(parsed);
}

size_t read_env_size_t(const char* name, size_t default_value, size_t min_value,
                       size_t max_value) {
  const char* raw = std::getenv(name);
  if (!raw || raw[0] == '\0') {
    return default_value;
  }

  char* end = nullptr;
  errno = 0;
  const unsigned long long parsed = std::strtoull(raw, &end, 10);
  if (errno != 0 || end == raw || (end && *end != '\0')) {
    return default_value;
  }

  if (parsed < static_cast<unsigned long long>(min_value) ||
      parsed > static_cast<unsigned long long>(max_value)) {
    return default_value;
  }

  return static_cast<size_t>(parsed);
}

int scaled_workload(int base_value, int scale_pct) {
  const int scaled = static_cast<int>((static_cast<int64_t>(base_value) * scale_pct) / 100);
  return scaled > 0 ? scaled : 1;
}

WorkloadConfig load_workload_config() {
  const int scale_pct = read_env_int("ZCOROUTINE_PERF_SCALE_PCT", 100, 1, 1000);

  WorkloadConfig config;
  config.scheduler_count =
      read_env_int("ZCOROUTINE_PERF_SCHED_COUNT", kDefaultSchedulerCount, 1, 256);
  config.producer_threads =
      read_env_int("ZCOROUTINE_PERF_PRODUCER_THREADS", kDefaultProducerThreads, 1, 256);
  config.scheduler_tasks = scaled_workload(
      read_env_int("ZCOROUTINE_PERF_SCHED_TASKS", kDefaultSchedulerTasks, 1, INT_MAX), scale_pct);
  config.yield_interval =
      read_env_int("ZCOROUTINE_PERF_YIELD_INTERVAL", kDefaultYieldInterval, 1, INT_MAX);
  config.stack_size =
      read_env_size_t("ZCOROUTINE_PERF_STACK_SIZE", kDefaultStackSize, 16 * 1024,
                      8 * 1024 * 1024);
  config.shared_stack_num =
      read_env_size_t("ZCOROUTINE_PERF_SHARED_STACK_NUM", kDefaultSharedStackNum, 1, 1024);
  config.channel_messages = scaled_workload(
      read_env_int("ZCOROUTINE_PERF_CHANNEL_MESSAGES", kDefaultChannelMessages, 1, INT_MAX),
      scale_pct);
  config.timer_tasks =
      scaled_workload(read_env_int("ZCOROUTINE_PERF_TIMER_TASKS", kDefaultTimerTasks, 1, INT_MAX),
                      scale_pct);
  config.hook_rounds =
      scaled_workload(read_env_int("ZCOROUTINE_PERF_HOOK_ROUNDS", kDefaultHookRounds, 1, INT_MAX),
                      scale_pct);
  return config;
}

void print_workload_config(const WorkloadConfig& config) {
  std::cout << "[zcoroutine-perf] config"
            << " scheduler_count=" << config.scheduler_count
            << " producer_threads=" << config.producer_threads
            << " scheduler_tasks=" << config.scheduler_tasks
            << " channel_messages=" << config.channel_messages
            << " timer_tasks=" << config.timer_tasks
            << " hook_rounds=" << config.hook_rounds
            << " yield_interval=" << config.yield_interval
            << " stack_size=" << config.stack_size
            << " shared_stack_num=" << config.shared_stack_num << std::endl;
}

void disable_benchmark_logging() {
  LoggerInitOptions options;
  options.level = zlog::LogLevel::value::OFF;
  options.async = false;
  options.formatter = kDefaultFormatter;
  options.sink = "stdout";
  init_logger(options);
}

void prepare_runtime(StackModel model, const WorkloadConfig& config) {
  shutdown();
  co_stack_model(model);
  co_stack_size(config.stack_size);
  co_stack_num(config.shared_stack_num);
  init(static_cast<uint32_t>(config.scheduler_count));
}

ScenarioResult run_scheduler_throughput(StackModel model, const WorkloadConfig& config) {
  prepare_runtime(model, config);
  RuntimeScenarioGuard guard;

  WaitGroup done(static_cast<uint32_t>(config.scheduler_tasks));
  std::atomic<int> executed(0);

  auto start = std::chrono::steady_clock::now();

  std::vector<std::thread> producers;
  producers.reserve(static_cast<size_t>(config.producer_threads));

  for (int t = 0; t < config.producer_threads; ++t) {
    producers.emplace_back([t, &done, &executed, &config]() {
      const int begin = (config.scheduler_tasks * t) / config.producer_threads;
      const int end = (config.scheduler_tasks * (t + 1)) / config.producer_threads;
      for (int i = begin; i < end; ++i) {
        go([&done, &executed, i, &config]() {
          if ((i % config.yield_interval) == 0) {
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
  const auto end = std::chrono::steady_clock::now();
  const int executed_count = executed.load(std::memory_order_relaxed);
  require_eq(executed_count, config.scheduler_tasks, "scheduler_submit executed mismatch");

  const double seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
  require_positive(seconds, "scheduler_submit elapsed must be positive");

  return ScenarioResult{"scheduler_submit", model, config.producer_threads,
                        config.scheduler_tasks, seconds,
                        static_cast<double>(config.scheduler_tasks) / seconds};
}

ScenarioResult run_channel_throughput(StackModel model, const WorkloadConfig& config) {
  prepare_runtime(model, config);
  RuntimeScenarioGuard guard;

  constexpr int kProducerCoroutines = 1;
  constexpr int kConsumerCoroutines = 1;
  constexpr uint32_t kChannelIoTimeoutMs = 100;

  Channel<int> channel(1024);
  WaitGroup done(kProducerCoroutines + kConsumerCoroutines);

  std::atomic<int> produced(0);
  std::atomic<int> consumed(0);
  std::atomic<bool> producer_ok(true);
  std::atomic<bool> consumer_ok(true);

  const auto start = std::chrono::steady_clock::now();

  go([&channel, &done, &produced, &producer_ok, &config]() {
    for (int i = 0; i < config.channel_messages; ++i) {
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

  go([&channel, &done, &consumed, &consumer_ok]() {
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
  const auto end = std::chrono::steady_clock::now();

  require_true(producer_ok.load(std::memory_order_acquire), "channel producer failed");
  require_true(consumer_ok.load(std::memory_order_acquire), "channel consumer failed");
  require_eq(produced.load(std::memory_order_relaxed), config.channel_messages,
             "channel produced count mismatch");
  require_eq(consumed.load(std::memory_order_relaxed), config.channel_messages,
             "channel consumed count mismatch");

  const double seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
  require_positive(seconds, "channel throughput elapsed must be positive");

  return ScenarioResult{"channel_spsc", model, kProducerCoroutines + kConsumerCoroutines,
                        config.channel_messages, seconds,
                        static_cast<double>(config.channel_messages) / seconds};
}

ScenarioResult run_timer_throughput(StackModel model, const WorkloadConfig& config) {
  prepare_runtime(model, config);
  RuntimeScenarioGuard guard;

  WaitGroup done(static_cast<uint32_t>(config.timer_tasks));
  std::atomic<int> executed(0);

  const auto start = std::chrono::steady_clock::now();

  for (int i = 0; i < config.timer_tasks; ++i) {
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
  const auto end = std::chrono::steady_clock::now();

  require_eq(executed.load(std::memory_order_relaxed), config.timer_tasks,
             "timer executed count mismatch");

  const double seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
  require_positive(seconds, "timer throughput elapsed must be positive");

  return ScenarioResult{"timer_sleep", model, config.scheduler_count, config.timer_tasks, seconds,
                        static_cast<double>(config.timer_tasks) / seconds};
}

ScenarioResult run_hook_io_throughput(StackModel model, const WorkloadConfig& config) {
  prepare_runtime(model, config);
  RuntimeScenarioGuard guard;

  int stream_pair[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, stream_pair) != 0) {
    fail("socketpair failed for hook perf scenario");
  }

  ScopedFd writer(stream_pair[0]);
  ScopedFd reader(stream_pair[1]);

  if (::fcntl(writer.get(), F_SETFL, ::fcntl(writer.get(), F_GETFL, 0) | O_NONBLOCK) != 0 ||
      ::fcntl(reader.get(), F_SETFL, ::fcntl(reader.get(), F_GETFL, 0) | O_NONBLOCK) != 0) {
    fail("failed to set non-blocking mode for hook perf scenario");
  }

  WaitGroup done(2);
  std::atomic<int> verified(0);
  std::atomic<bool> writer_ok(true);
  std::atomic<bool> reader_ok(true);

  const auto start = std::chrono::steady_clock::now();

  go([&done, &writer_ok, fd = writer.get(), &config]() {
    for (int i = 0; i < config.hook_rounds; ++i) {
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

  go([&done, &reader_ok, &verified, fd = reader.get(), &config]() {
    for (int i = 0; i < config.hook_rounds; ++i) {
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
  const auto end = std::chrono::steady_clock::now();

  require_true(writer_ok.load(std::memory_order_acquire), "hook writer failed");
  require_true(reader_ok.load(std::memory_order_acquire), "hook reader failed");
  require_eq(verified.load(std::memory_order_relaxed), config.hook_rounds,
             "hook verified count mismatch");

  const double seconds =
      std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();
  require_positive(seconds, "hook throughput elapsed must be positive");

  return ScenarioResult{"hook_socketpair", model, 2, config.hook_rounds, seconds,
                        static_cast<double>(config.hook_rounds) / seconds};
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

int run_benchmark() {
  disable_benchmark_logging();

  const WorkloadConfig config = load_workload_config();
  print_workload_config(config);

  std::vector<ScenarioResult> results;
  results.push_back(run_scheduler_throughput(StackModel::kShared, config));
  results.push_back(run_scheduler_throughput(StackModel::kIndependent, config));
  results.push_back(run_channel_throughput(StackModel::kShared, config));
  results.push_back(run_timer_throughput(StackModel::kShared, config));
  results.push_back(run_hook_io_throughput(StackModel::kShared, config));

  for (size_t i = 0; i < results.size(); ++i) {
    require_positive(results[i].throughput_ops_per_second,
                     std::string("non-positive throughput for scenario ") + results[i].scenario);
    print_result(results[i]);
  }

  return 0;
}

}  // namespace

int run_stack_model_perf_main() {
  try {
    return run_benchmark();
  } catch (const std::exception& ex) {
    shutdown();
    std::cerr << "[zcoroutine-perf] error=" << ex.what() << std::endl;
    return 1;
  } catch (...) {
    shutdown();
    std::cerr << "[zcoroutine-perf] error=unknown_exception" << std::endl;
    return 1;
  }
}

}  // namespace zcoroutine

int main() { return zcoroutine::run_stack_model_perf_main(); }
