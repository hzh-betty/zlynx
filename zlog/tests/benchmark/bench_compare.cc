
#include "logger.h"

#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// spdlog
#include <spdlog/async.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

// glog
#include <glog/logging.h>

// 递归删除目录
void remove_directory(const char *path) {
  DIR *dir = opendir(path);
  if (dir) {
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
        continue;
      }
      std::string full_path = std::string(path) + "/" + entry->d_name;
      struct stat st;
      if (stat(full_path.c_str(), &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
          remove_directory(full_path.c_str());
        } else {
          unlink(full_path.c_str());
        }
      }
    }
    closedir(dir);
  }
  rmdir(path);
}

// 确保日志目录存在
void prepare_log_dir() {
  struct stat st = {0};
  if (stat("bench_logs", &st) == -1) {
    mkdir("bench_logs", 0755);
  }
}

// 清理日志目录
void cleanup_log_dir() { remove_directory("bench_logs"); }

// 生成指定长度的日志内容
std::string make_string(size_t len) { return std::string(len, 'x'); }

// 测试结果结构体
struct BenchmarkResult {
  std::string logger_type; // 日志类型
  int thread_count;        // 线程数
  size_t message_size;     // 每条日志字节数
  size_t total_messages;   // 输出日志数量
  double duration;         // 耗时（秒）
  double throughput_mbps;  // 吞吐量（MB/s）
};

// 全局结果容器
std::vector<BenchmarkResult> g_results;

// 固定时长测试（3秒）
template <typename LogFunc>
BenchmarkResult run_timed_benchmark(const std::string &logger_type,
                                    int thread_count, size_t message_size,
                                    LogFunc log_func) {
  const auto test_duration = std::chrono::seconds(3);
  std::atomic<size_t> total_messages{0};
  std::atomic<bool> stop_flag{false};

  std::vector<std::thread> threads;
  auto start_time = std::chrono::steady_clock::now();

  // 启动测试线程
  for (int i = 0; i < thread_count; ++i) {
    threads.emplace_back([&, i]() {
      size_t local_count = 0;
      while (!stop_flag.load(std::memory_order_relaxed)) {
        log_func(i);
        ++local_count;
      }
      total_messages.fetch_add(local_count, std::memory_order_relaxed);
    });
  }

  // 等待3秒
  std::this_thread::sleep_for(test_duration);
  stop_flag.store(true, std::memory_order_relaxed);

  // 等待所有线程结束
  for (auto &t : threads) {
    t.join();
  }

  auto end_time = std::chrono::steady_clock::now();
  double duration =
      std::chrono::duration<double>(end_time - start_time).count();

  size_t total = total_messages.load();
  double total_bytes = static_cast<double>(total * message_size);
  double throughput_mbps = (total_bytes / (1024.0 * 1024.0)) / duration;

  return BenchmarkResult{logger_type, thread_count, message_size,
                         total,       duration,     throughput_mbps};
}

static const std::string kBenchPattern = "%m";

// Zlog 同步模式测试
void test_zlog_sync(int thread_count, size_t message_size) {
  prepare_log_dir();

  auto formatter = std::make_shared<zlog::Formatter>(kBenchPattern);
  std::vector<zlog::LogSink::ptr> sinks;
  sinks.push_back(std::make_shared<zlog::FileSink>("bench_logs/zlog_sync.log"));

  // 所有线程共用一个 logger
  auto logger = std::make_shared<zlog::SyncLogger>(
      "bench_sync", zlog::LogLevel::value::INFO, formatter, sinks);

  std::string msg = make_string(message_size);

  auto result = run_timed_benchmark(
      "Zlog-Sync", thread_count, message_size, [&](int thread_id) {
        (void)thread_id;
        logger->logImpl(zlog::LogLevel::value::INFO, "", 0, msg.c_str());
      });

  g_results.push_back(result);
  cleanup_log_dir();
  std::this_thread::sleep_for(std::chrono::seconds(1));
}

// Zlog 异步模式 (ASYNC_SAFE) 测试
void test_zlog_async_safe(int thread_count, size_t message_size) {
  prepare_log_dir();

  auto formatter = std::make_shared<zlog::Formatter>(kBenchPattern);
  std::vector<zlog::LogSink::ptr> sinks;
  sinks.push_back(
      std::make_shared<zlog::FileSink>("bench_logs/zlog_async_safe.log"));

  // 所有线程共用一个 logger
  auto logger = std::make_shared<zlog::AsyncLogger>(
      "bench_async_safe", zlog::LogLevel::value::INFO, formatter, sinks,
      zlog::AsyncType::ASYNC_SAFE, std::chrono::milliseconds(100));

  std::string msg = make_string(message_size);

  auto result = run_timed_benchmark(
      "Zlog-Async-Safe", thread_count, message_size, [&](int thread_id) {
        (void)thread_id;
        logger->logImpl(zlog::LogLevel::value::INFO, "", 0, msg.c_str());
      });

  g_results.push_back(result);
  logger.reset();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  cleanup_log_dir();
  std::this_thread::sleep_for(std::chrono::seconds(1));
}

// Zlog 异步模式 (ASYNC_UNSAFE) 测试
void test_zlog_async_unsafe(int thread_count, size_t message_size) {
  prepare_log_dir();

  auto formatter = std::make_shared<zlog::Formatter>(kBenchPattern);
  std::vector<zlog::LogSink::ptr> sinks;
  sinks.push_back(
      std::make_shared<zlog::FileSink>("bench_logs/zlog_async_unsafe.log"));

  // 所有线程共用一个 logger
  auto logger = std::make_shared<zlog::AsyncLogger>(
      "bench_async_unsafe", zlog::LogLevel::value::INFO, formatter, sinks,
      zlog::AsyncType::ASYNC_UNSAFE, std::chrono::milliseconds(100));

  std::string msg = make_string(message_size);

  auto result = run_timed_benchmark(
      "Zlog-Async-Unsafe", thread_count, message_size, [&](int thread_id) {
        (void)thread_id;
        logger->logImpl(zlog::LogLevel::value::INFO, "", 0, msg.c_str());
      });

  g_results.push_back(result);
  logger.reset();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  cleanup_log_dir();
  std::this_thread::sleep_for(std::chrono::seconds(1));
}

// Spdlog 同步模式测试
void test_spdlog_sync(int thread_count, size_t message_size) {
  prepare_log_dir();

  // 所有线程共用一个 logger
  std::shared_ptr<spdlog::logger> logger;
  try {
    logger = spdlog::basic_logger_mt("bench_spd_sync",
                                     "bench_logs/spdlog_sync.log", true);
    logger->set_pattern("%v");
  } catch (...) {
    logger = spdlog::get("bench_spd_sync");
  }

  std::string msg = make_string(message_size);

  auto result = run_timed_benchmark("Spdlog-Sync", thread_count, message_size,
                                    [&](int thread_id) {
                                      (void)thread_id;
                                      logger->info(msg);
                                    });

  g_results.push_back(result);
  spdlog::drop_all();
  cleanup_log_dir();
  std::this_thread::sleep_for(std::chrono::seconds(1));
}

// Spdlog 异步模式测试
void test_spdlog_async(int thread_count, size_t message_size) {
  prepare_log_dir();

  static std::once_flag pool_flag;
  std::call_once(pool_flag, []() { spdlog::init_thread_pool(8192, 1); });

  // 所有线程共用一个 logger
  std::shared_ptr<spdlog::logger> logger;
  try {
    logger = spdlog::basic_logger_mt<spdlog::async_factory>(
        "bench_spd_async", "bench_logs/spdlog_async.log", true);
    logger->set_pattern("%v");
  } catch (...) {
    logger = spdlog::get("bench_spd_async");
  }

  std::string msg = make_string(message_size);

  auto result = run_timed_benchmark("Spdlog-Async", thread_count, message_size,
                                    [&](int thread_id) {
                                      (void)thread_id;
                                      logger->info(msg);
                                    });

  g_results.push_back(result);
  spdlog::drop_all();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  cleanup_log_dir();
  std::this_thread::sleep_for(std::chrono::seconds(1));
}

// Glog 同步模式测试 (仅支持同步)
void test_glog(int thread_count, size_t message_size) {
  prepare_log_dir();

  static bool glog_init = false;
  if (!glog_init) {
    google::InitGoogleLogging("bench_glog");
    FLAGS_logtostderr = 0;
    FLAGS_alsologtostderr = 0;
    FLAGS_log_dir = "bench_logs";
    glog_init = true;
  }

  std::string msg = make_string(message_size);

  auto result = run_timed_benchmark("Glog-Sync", thread_count, message_size,
                                    [&](int thread_id) { LOG(INFO) << msg; });

  g_results.push_back(result);
  google::FlushLogFiles(google::INFO);
  cleanup_log_dir();
  std::this_thread::sleep_for(std::chrono::seconds(1));
}

// 打印结果表格
void print_results() {
  std::cout << "\n"
            << "========================================="
               "=========================================="
               "===================\n";
  std::cout << std::left << std::setw(22) << "Logger Type" << std::setw(10)
            << "Threads" << std::setw(18) << "Total Messages" << std::setw(15)
            << "Msg Size(B)" << std::setw(15) << "Duration(s)" << std::setw(18)
            << "Throughput(MB/s)" << "\n";
  std::cout << "========================================="
               "=========================================="
               "===================\n";

  for (const auto &r : g_results) {
    std::cout << std::left << std::setw(22) << r.logger_type << std::setw(10)
              << r.thread_count << std::setw(18) << r.total_messages
              << std::setw(15) << r.message_size << std::setw(15) << std::fixed
              << std::setprecision(2) << r.duration << std::setw(18)
              << std::fixed << std::setprecision(2) << r.throughput_mbps
              << "\n";
  }

  std::cout << "========================================="
               "=========================================="
               "===================\n";
}

int main() {
  std::cout << "Starting performance benchmark...\n";
  std::cout << "Each test runs for 3 seconds\n";
  std::cout << "Thread counts: 1, 2, 4, 8, 16\n";
  std::cout << "Message sizes: 32, 64, 128, 256, 512 bytes\n\n";

  const std::vector<int> thread_counts = {1, 2, 4, 8, 16};
  const std::vector<size_t> message_sizes = {32, 64, 128, 256, 512};

  // 测试所有组合
  for (int threads : thread_counts) {
    for (size_t msg_size : message_sizes) {
      std::cout << "[" << threads << " threads, " << msg_size << " bytes] ";

      // Zlog 同步
      std::cout << "Zlog-Sync...";
      test_zlog_sync(threads, msg_size);

      // Zlog 异步 Safe
      std::cout << " Zlog-Async-Safe...";
      test_zlog_async_safe(threads, msg_size);

      // Zlog 异步 Unsafe
      std::cout << " Zlog-Async-Unsafe...";
      test_zlog_async_unsafe(threads, msg_size);

      // Spdlog 同步
      std::cout << " Spdlog-Sync...";
      test_spdlog_sync(threads, msg_size);

      // Spdlog 异步
      std::cout << " Spdlog-Async...";
      test_spdlog_async(threads, msg_size);

      // Glog
      std::cout << " Glog-Sync...";
      test_glog(threads, msg_size);

      std::cout << " Done\n";
    }
  }

  // 打印结果表格
  print_results();

  std::cout << "\nBenchmark completed!\n";
  return 0;
}
